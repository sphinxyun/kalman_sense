#include "QuadUkf.h"

QuadUkf::QuadUkf(ros::Publisher poseStampedPub,
                 ros::Publisher poseWithCovStampedPub,
                 ros::Publisher poseArrayPub)
{
  poseStampedPublisher = poseStampedPub;
  poseWithCovStampedPublisher = poseWithCovStampedPub;
  poseArrayPublisher = poseArrayPub;

  numStates = 16;
  numSensors = 10;
  this->UnscentedKf::setWeights();

  // Define initial position, quaternion, velocity, angular velocity, and
  // acceleration.
  Eigen::Quaterniond initQuat = Eigen::Quaterniond::Identity();
  Eigen::Vector3d initPosition, initVelocity, initAngVel, initAcceleration;
  initPosition << 0, 0, 1;  // "one meter above the origin"
  initVelocity = Eigen::Vector3d::Zero();
  initAngVel = Eigen::Vector3d::Zero();
  initAcceleration = Eigen::Vector3d::Zero();

  // Define initial belief
  QuadUkf::QuadState initState {initPosition, initQuat, initVelocity,
                                initAngVel, initAcceleration};
  Eigen::MatrixXd initCov = Eigen::MatrixXd::Identity(numStates, numStates);
  initCov = initCov * 0.01;
  double initTimeStamp = ros::Time::now().toSec();
  double init_dt = 0.0001;
  QuadUkf::QuadBelief initBelief {initTimeStamp, init_dt, initState, initCov};
  lastBelief = initBelief;

  // Initialize process noise covariance and sensor noise covariance
  Q_ProcNoiseCov = Eigen::MatrixXd::Identity(numStates, numStates);
  Q_ProcNoiseCov *= 0.01;  // default value
  R_SensorNoiseCov = Eigen::MatrixXd::Identity(numSensors, numSensors);
  R_SensorNoiseCov *= 0.01;  // default value

  H_SensorMap = Eigen::MatrixXd::Zero(numStates, numSensors);
  H_SensorMap.block(0, 0, numSensors, numSensors) = Eigen::MatrixXd::Identity(
      numSensors, numSensors);

  lastPoseMsg.header.stamp.sec = initTimeStamp;
  lastPoseMsg.pose.pose.position.x = initPosition(0);
  lastPoseMsg.pose.pose.position.y = initPosition(1);
  lastPoseMsg.pose.pose.position.z = initPosition(2);

  quadPoseArray.poses.clear();
  quadPoseArray.header.frame_id = "map";
  quadPoseArray.header.stamp = ros::Time();
}

QuadUkf::QuadUkf(QuadUkf&& other)
{
  std::lock_guard<std::timed_mutex> lock(other.mtx);

  Q_ProcNoiseCov = std::move(other.Q_ProcNoiseCov);
  other.Q_ProcNoiseCov = Eigen::MatrixXd::Zero(1, 1);

  R_SensorNoiseCov = std::move(other.R_SensorNoiseCov);
  other.R_SensorNoiseCov = Eigen::MatrixXd::Zero(1, 1);

  ros::NodeHandle n;
  poseStampedPublisher = std::move(other.poseStampedPublisher);
  poseWithCovStampedPublisher = std::move(other.poseWithCovStampedPublisher);
  poseArrayPublisher = std::move(other.poseArrayPublisher);
  ros::Publisher p = n.advertise<std_msgs::Empty>("empty", 1);
  other.poseWithCovStampedPublisher = p;
  other.poseArrayPublisher = p;

  H_SensorMap = std::move(other.H_SensorMap);
  other.H_SensorMap = Eigen::MatrixXd::Zero(1, 1);
}

QuadUkf::~QuadUkf()
{
}

/*
 * Predicts next state based on IMU readings, resets lastBelief to
 * reflect that prediction, then publishes lastBelief as a
 * geometry_msgs::PoseWithCovarianceStamped message.
 */
void QuadUkf::imuCallback(const sensor_msgs::ImuConstPtr &msg_in)
{
  mtx.try_lock_for(std::chrono::milliseconds(100));

  QuadBelief xB = lastBelief;
  xB.state.angular_velocity(0) = msg_in->angular_velocity.x;
  xB.state.angular_velocity(1) = -msg_in->angular_velocity.y;
  xB.state.angular_velocity(2) = msg_in->angular_velocity.z;
  xB.state.acceleration(0) = -msg_in->linear_acceleration.x;
  xB.state.acceleration(1) = msg_in->linear_acceleration.y;
  xB.state.acceleration(2) = msg_in->linear_acceleration.z;

  // Remove gravity
  xB.state.acceleration = xB.state.acceleration
      - xB.state.quaternion.toRotationMatrix().inverse() * GRAVITY_ACCEL;

  // Predict next state and reset lastBelief
  Eigen::VectorXd x = quadStateToEigen(xB.state);
  xB.dt = msg_in->header.stamp.toSec() - lastBelief.timeStamp;
  UnscentedKf::Belief b = predictState(x, xB.covariance, Q_ProcNoiseCov, xB.dt);
  QuadUkf::QuadBelief qb {msg_in->header.stamp.toSec(), xB.dt, eigenToQuadState(
      b.state),
                          b.covariance};
  qb.state.quaternion = checkQuat(lastBelief.state.quaternion,
                                  qb.state.quaternion);
  lastBelief = qb;

  publishAllPoseMessages(lastBelief);

  mtx.unlock();
}

/*
 * Corrects state based on pose sensor readings (that is, SLAM) and resets
 * lastBelief. Then it publishes lastBelief.
 */
void QuadUkf::poseCallback(
    const geometry_msgs::PoseWithCovarianceStampedConstPtr &msg_in)
{
  mtx.try_lock_for(std::chrono::milliseconds(100));

  Eigen::VectorXd z(numSensors);
  z(POS_X) = -msg_in->pose.pose.position.x;
  z(POS_Y) = msg_in->pose.pose.position.y;
  z(POS_Z) = msg_in->pose.pose.position.z;
  z(QUAT_X) = msg_in->pose.pose.orientation.w;
  z(QUAT_Y) = -msg_in->pose.pose.orientation.z;
  z(QUAT_Z) = msg_in->pose.pose.orientation.y;
  z(QUAT_W) = msg_in->pose.pose.orientation.x;
  double dtPose = msg_in->header.stamp.toSec() - lastPoseMsg.header.stamp.sec;
  z(VEL_X) = (z(POS_X) - lastPoseMsg.pose.pose.position.x) / dtPose;
  z(VEL_Y) = (z(POS_Y) - lastPoseMsg.pose.pose.position.y) / dtPose;
  z(VEL_Z) = (z(POS_Z) - lastPoseMsg.pose.pose.position.z) / dtPose;

  // Check incoming quaternion for rotational continuity and replace if not
  // continuous.
  Eigen::Quaterniond inQuat;
  inQuat.x() = z(QUAT_X);
  inQuat.y() = z(QUAT_Y);
  inQuat.z() = z(QUAT_Z);
  inQuat.w() = z(QUAT_W);
  Eigen::Vector4d chosenQuat =
      checkQuat(lastBelief.state.quaternion, inQuat).coeffs();
  z.block<4, 1>(3, 0) = chosenQuat;

  // Set time step "dt".
  double dt = msg_in->header.stamp.toSec() - lastBelief.timeStamp;

  // Correct the state and covariance at time of pose message
  QuadUkf::QuadBelief xB = lastBelief;
  xB.state.velocity = lastBelief.state.velocity
      + lastBelief.state.acceleration * dt;
  xB.state.position = (xB.state.velocity + lastBelief.state.velocity) / 2.0 * dt
      + lastBelief.state.position;
  Eigen::MatrixXd Omega = generateBigOmegaMat(
      lastBelief.state.angular_velocity);
  xB.state.quaternion.coeffs() = lastBelief.state.quaternion.coeffs()
      + 0.5 * Omega * lastBelief.state.quaternion.coeffs() * dt;
  xB.state.quaternion.normalize();
  Eigen::VectorXd xPred = quadStateToEigen(xB.state);
  Eigen::MatrixXd P = lastBelief.covariance;
  UnscentedKf::Belief currStateAndCov = correctState(xPred, P, z,
                                                     R_SensorNoiseCov);

  // Update lastBelief.
  lastBelief.dt = dt;
  lastBelief.state = eigenToQuadState(currStateAndCov.state);
  lastBelief.covariance = currStateAndCov.covariance;
  lastBelief.timeStamp = msg_in->header.stamp.toSec();

  publishAllPoseMessages(lastBelief);

  mtx.unlock();
}

void QuadUkf::publishAllPoseMessages(QuadUkf::QuadBelief b)
{
  geometry_msgs::PoseWithCovarianceStamped pwcs =
      quadBeliefToPoseWithCovStamped(b);
  poseWithCovStampedPublisher.publish(pwcs);
  updatePoseArray(pwcs);
  geometry_msgs::PoseStamped ps = quadBeliefToPoseStamped(b);
  poseStampedPublisher.publish(ps);
}

/*
 * Puts a given pose into the first position of quadPoseArray. Once
 * quadPoseArray reaches POSE_ARRAY_SIZE, the last pose is popped on each call.
 * After these operations are performed, this function publishes
 * quadPoseArray.
 */
void QuadUkf::updatePoseArray(const geometry_msgs::PoseWithCovarianceStamped p)
{
  quadPoseArray.poses.insert(quadPoseArray.poses.begin(), 1, p.pose.pose);
  if (quadPoseArray.poses.size() > POSE_ARRAY_SIZE)
  {
    quadPoseArray.poses.pop_back();
  }
  poseArrayPublisher.publish(quadPoseArray);
}

/*
 * Ensures rotational continuity by checking for sign-flipping during large
 * rotations (greater than about 270 degrees).
 */
Eigen::Quaterniond QuadUkf::checkQuat(const Eigen::Quaterniond lastQuat,
                                      const Eigen::Quaterniond nextQuat) const
{
  Eigen::Vector4d lastVec, nextVec;
  lastVec = lastQuat.normalized().coeffs();
  nextVec = nextQuat.normalized().coeffs();

  double sum = (lastVec + nextVec).norm();
  double diff = (lastVec - nextVec).norm();

  Eigen::Quaterniond out;
  if (sum > diff)
  {
    out.coeffs() = nextVec;
  }
  else
  {
    out.coeffs() = -nextVec;
  }
  return out;
}

geometry_msgs::PoseStamped QuadUkf::quadBeliefToPoseStamped(
    const QuadUkf::QuadBelief b) const
{
  geometry_msgs::PoseStamped p;
  p.header.stamp.sec = b.timeStamp;
  p.header.stamp.nsec = (b.timeStamp - floor(b.timeStamp)) * pow(10, 9);
  p.pose.position.x = b.state.position(0);
  p.pose.position.y = b.state.position(1);
  p.pose.position.z = b.state.position(2);
  p.pose.orientation.w = b.state.quaternion.w();
  p.pose.orientation.x = b.state.quaternion.x();
  p.pose.orientation.y = b.state.quaternion.y();
  p.pose.orientation.z = b.state.quaternion.z();

  return p;
}

geometry_msgs::PoseWithCovarianceStamped QuadUkf::quadBeliefToPoseWithCovStamped(
    const QuadUkf::QuadBelief b) const
{
  geometry_msgs::PoseWithCovarianceStamped p;
  p.header.stamp.sec = b.timeStamp;
  p.header.stamp.nsec = (b.timeStamp - floor(b.timeStamp)) * pow(10, 9);
  p.pose.pose.position.x = b.state.position(0);
  p.pose.pose.position.y = b.state.position(1);
  p.pose.pose.position.z = b.state.position(2);
  p.pose.pose.orientation.w = b.state.quaternion.w();
  p.pose.pose.orientation.x = b.state.quaternion.x();
  p.pose.pose.orientation.y = b.state.quaternion.y();
  p.pose.pose.orientation.z = b.state.quaternion.z();

  // Copy covariance matrix from b into the covariance array in p
  Eigen::MatrixXd covMat = b.covariance.block<6, 6>(0, 0);
  for (int i = 0; i < covMat.rows() * covMat.cols(); ++i)
  {
    p.pose.covariance[i] = covMat(i);
  }

  return p;
}

Eigen::VectorXd QuadUkf::processFunc(const Eigen::VectorXd x, const double dt)
{
  QuadUkf::QuadState prevState = eigenToQuadState(x);
  prevState.quaternion.normalize();
  QuadUkf::QuadState currState;

  // Compute current orientation via quaternion integration.
  Eigen::MatrixXd Omega = generateBigOmegaMat(prevState.angular_velocity);
  currState.quaternion.coeffs() = prevState.quaternion.coeffs()
      + 0.5 * Omega * prevState.quaternion.coeffs() * dt;
  currState.quaternion.normalize();

  // Rotate current and previous accelerations into inertial frame, then
  // average them.
  currState.acceleration = prevState.quaternion.toRotationMatrix()
      * prevState.acceleration;

  // Compute current velocity by integrating current acceleration.
  currState.velocity = prevState.velocity
      + 0.5 * (lastBelief.state.acceleration + currState.acceleration) * dt;

  // Compute current position by integrating current velocity.
  currState.position = prevState.position
      + ((currState.velocity + prevState.velocity) / 2.0) * dt;

  // Angular velocity is assumed to be correct as measured.
  currState.angular_velocity = prevState.angular_velocity;

  return quadStateToEigen(currState);
}

Eigen::VectorXd QuadUkf::observationFunc(const Eigen::VectorXd stateVec)
{
  return stateVec.head(numSensors);
}

/*
 * Returns the 4-by-4 Big Omega matrix for performing quaternion integration,
 * given a vector "w" of angular velocities in radians per second.
 */
Eigen::MatrixXd QuadUkf::generateBigOmegaMat(const Eigen::Vector3d w) const
{
  Eigen::MatrixXd Omega(4, 4);

  // Upper left 3-by-3 block: negative skew-symmetric matrix of vector w
  Omega(0, 0) = 0;
  Omega(0, 1) = w(2);
  Omega(0, 2) = -w(1);

  Omega(1, 0) = -w(2);
  Omega(1, 1) = 0;
  Omega(1, 2) = w(0);

  Omega(2, 0) = w(1);
  Omega(2, 1) = -w(0);
  Omega(2, 2) = 0;

  // Bottom left 1-by-3 block: negative transpose of vector w
  Omega.block<1, 3>(3, 0) = -w.transpose();

  // Upper right 3-by-1 block: w
  Omega.block<3, 1>(0, 3) = w;

  // Bottom right 1-by-1 block: 0
  Omega(3, 3) = 0;

  return Omega;
}

Eigen::VectorXd QuadUkf::quadStateToEigen(const QuadUkf::QuadState qs) const
{
  Eigen::VectorXd x(numStates);

  x(POS_X) = qs.position(0);
  x(POS_Y) = qs.position(1);
  x(POS_Z) = qs.position(2);

  x(QUAT_X) = qs.quaternion.x();
  x(QUAT_Y) = qs.quaternion.y();
  x(QUAT_Z) = qs.quaternion.z();
  x(QUAT_W) = qs.quaternion.w();

  x(VEL_X) = qs.velocity(0);
  x(VEL_Y) = qs.velocity(1);
  x(VEL_Z) = qs.velocity(2);

  x(ANGVEL_X) = qs.angular_velocity(0);
  x(ANGVEL_Y) = qs.angular_velocity(1);
  x(ANGVEL_Z) = qs.angular_velocity(2);

  x(ACCEL_X) = qs.acceleration(0);
  x(ACCEL_Y) = qs.acceleration(1);
  x(ACCEL_Z) = qs.acceleration(2);

  return x;
}

QuadUkf::QuadState QuadUkf::eigenToQuadState(const Eigen::VectorXd x) const
{
  QuadUkf::QuadState qs;

  qs.position(0) = x(POS_X);
  qs.position(1) = x(POS_Y);
  qs.position(2) = x(POS_Z);

  qs.quaternion.x() = x(QUAT_X);
  qs.quaternion.y() = x(QUAT_Y);
  qs.quaternion.z() = x(QUAT_Z);
  qs.quaternion.w() = x(QUAT_W);

  qs.velocity(0) = x(VEL_X);
  qs.velocity(1) = x(VEL_Y);
  qs.velocity(2) = x(VEL_Z);

  qs.angular_velocity(0) = x(ANGVEL_X);
  qs.angular_velocity(1) = x(ANGVEL_Y);
  qs.angular_velocity(2) = x(ANGVEL_Z);

  qs.acceleration(0) = x(ACCEL_X);
  qs.acceleration(1) = x(ACCEL_Y);
  qs.acceleration(2) = x(ACCEL_Z);

  return qs;
}
