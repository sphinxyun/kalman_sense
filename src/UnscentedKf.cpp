#include "UnscentedKf.h"

UnscentedKf::UnscentedKf() :
    _numStates(16)
{
}

UnscentedKf::~UnscentedKf()
{
}

UnscentedKf::Transform UnscentedKf::predictState(Eigen::VectorXd x,
                                                 Eigen::MatrixXd P,
                                                 Eigen::MatrixXd Q, double dt)
{
  int n = x.rows();
  double scalingCoeff = n + lambda;
  Eigen::MatrixXd sigmaPts(n, 2 * n + 1);
  sigmaPts = computeSigmaPoints(x, P, scalingCoeff);

  UnscentedKf::Transform tf = unscentedStateTransform(sigmaPts, meanWeights,
                                                      covarianceWeights, Q, dt);
  return tf;
}

UnscentedKf::Belief UnscentedKf::correctState(UnscentedKf::Transform stateTf,
                                              Eigen::VectorXd z,
                                              Eigen::MatrixXd R)
{
  //TODO change this function to simply take in state vector rather than stateTf
  Eigen::VectorXd xPred = stateTf.vector;
  int n = xPred.rows();
  int m = z.rows();

  UnscentedKf::Transform sensorTf = unscentedSensorTransform(
      m, stateTf.sigmaPoints, meanWeights, covarianceWeights, R);
  Eigen::VectorXd zPred = sensorTf.vector;  // Expected sensor vector
  Eigen::MatrixXd P_zz = sensorTf.covariance;  // Sensor/sensor covariance

  // Compute state/sensor cross-covariance
  Eigen::MatrixXd P_xz = Eigen::MatrixXd::Zero(n, m);
  P_xz = stateTf.deviations * covarianceWeights.asDiagonal()
      * sensorTf.deviations.transpose();

  // Compute Kalman gain
  Eigen::MatrixXd K = Eigen::MatrixXd::Zero(n, m);
  K = P_xz * P_zz.inverse();

  // Update state vector
  Eigen::VectorXd xCorr = Eigen::VectorXd::Zero(n);
  xCorr = xPred + K * (z - zPred);

  // Update state covariance
  Eigen::MatrixXd PPred = stateTf.covariance;
  Eigen::MatrixXd PCorr = Eigen::MatrixXd::Zero(n, n);
  PCorr = PPred - K * P_xz.transpose();
  //PCorr = PPred - K * P_zz * K.transpose()?

  UnscentedKf::Belief bel {xCorr, PCorr};
  return bel;
}

UnscentedKf::Belief UnscentedKf::run(Eigen::VectorXd x, Eigen::MatrixXd P,
                                     Eigen::VectorXd z, Eigen::MatrixXd Q,
                                     Eigen::MatrixXd R, double dt)
{
  UnscentedKf::Transform stateTf = predictState(x, P, Q, dt);
  UnscentedKf::Belief bel = correctState(stateTf, z, R);
  return bel;
}

UnscentedKf::Transform UnscentedKf::unscentedStateTransform(
    Eigen::MatrixXd sigmaPts, Eigen::VectorXd meanWts, Eigen::VectorXd covWts,
    Eigen::MatrixXd noiseCov, double dt)
{
  int n = sigmaPts.rows();
  int L = sigmaPts.cols();
  Eigen::VectorXd vec = Eigen::VectorXd::Zero(n);
  Eigen::MatrixXd sigmas = Eigen::MatrixXd::Zero(n, L);

  UnscentedKf::SigmaPointSet sample = sampleStateSpace(sigmaPts, meanWts, dt);

  vec = sample.vector;
  sigmas = sample.sigmaPoints;

  Eigen::MatrixXd devs = Eigen::MatrixXd::Zero(n, L);
  devs = computeDeviations(sample);

  Eigen::MatrixXd cov = Eigen::MatrixXd::Zero(n, n);
  cov = computeCovariance(devs, covWts, noiseCov);

  UnscentedKf::Transform out {vec, sigmas, cov, devs};
  return out;
}

UnscentedKf::Transform UnscentedKf::unscentedSensorTransform(
    int numSensors, Eigen::MatrixXd sigmaPts, Eigen::VectorXd meanWts,
    Eigen::VectorXd covWts, Eigen::MatrixXd noiseCov)
{
  int n = sigmaPts.rows();
  int L = sigmaPts.cols();
  Eigen::VectorXd vec = Eigen::VectorXd::Zero(n);
  Eigen::MatrixXd sigmas = Eigen::MatrixXd::Zero(n, L);
  Eigen::MatrixXd cov = Eigen::MatrixXd::Zero(n, n);
  Eigen::MatrixXd devs = Eigen::MatrixXd::Zero(n, L);

  UnscentedKf::SigmaPointSet sample = sampleSensorSpace(numSensors, sigmaPts,
                                                        meanWts);

  vec = sample.vector;
  sigmas = sample.sigmaPoints;

  devs = computeDeviations(sample);
  cov = computeCovariance(devs, covWts, noiseCov);

  UnscentedKf::Transform out {vec, sigmas, cov, devs};
  return out;
}

Eigen::MatrixXd UnscentedKf::computeSigmaPoints(Eigen::VectorXd x,
                                                Eigen::MatrixXd P,
                                                double scalingCoeff) const
{
  // Compute lower Cholesky factor "A" of the given covariance matrix P.
  Eigen::LDLT<Eigen::MatrixXd> ldltOfCovMat(P);
  Eigen::MatrixXd L = ldltOfCovMat.matrixL();
  Eigen::MatrixXd A = scalingCoeff * L;

  // Populate a matrix "Y", which is filled columnwise with the given column
  // vector x.
  int n = x.rows();
  Eigen::MatrixXd Y = Eigen::MatrixXd::Zero(n, n);
  Y = fillMatrixWithVector(x, n);

  Eigen::MatrixXd sigmaPts(n, 2 * n + 1);
  sigmaPts << x, Y + A, Y - A;
  return sigmaPts;
}

Eigen::MatrixXd UnscentedKf::computeDeviations(
    UnscentedKf::SigmaPointSet sample) const
{
  Eigen::VectorXd vec = sample.vector;
  Eigen::MatrixXd sigmaPts = sample.sigmaPoints;

  int numRows = sigmaPts.rows();
  int numCols = sigmaPts.cols();
  Eigen::MatrixXd vecMat = fillMatrixWithVector(vec, numCols);
  Eigen::MatrixXd devs = Eigen::MatrixXd::Zero(numRows, numCols);

  return sigmaPts - vecMat;
}

Eigen::MatrixXd UnscentedKf::computeCovariance(Eigen::MatrixXd deviations,
                                               Eigen::VectorXd covWts,
                                               Eigen::MatrixXd noiseCov) const
{
  return deviations * covWts.asDiagonal() * deviations.transpose() + noiseCov;
}

UnscentedKf::SigmaPointSet UnscentedKf::sampleStateSpace(
    Eigen::MatrixXd sigmaPts, Eigen::VectorXd meanWts, double dt)
{
  int n = sigmaPts.rows();
  int L = sigmaPts.cols();

  Eigen::VectorXd vec = Eigen::VectorXd::Zero(n);
  Eigen::MatrixXd sigmas = Eigen::MatrixXd::Zero(n, L);
  for (int i = 0; i < L; i++)
  {
    sigmas.col(i) = processFunc(sigmaPts.col(i), dt);
    vec += meanWts(i) * sigmas.col(i);
  }

  UnscentedKf::SigmaPointSet out {vec, sigmas};
  return out;
}

UnscentedKf::SigmaPointSet UnscentedKf::sampleSensorSpace(
    int numSensors, Eigen::MatrixXd sigmaPts, Eigen::VectorXd meanWts)
{
  int n = numSensors;
  int L = sigmaPts.cols();

  Eigen::VectorXd vec = Eigen::VectorXd::Zero(n);
  Eigen::MatrixXd sigmas = Eigen::MatrixXd::Zero(n, L);
  for (int i = 0; i < L; i++)
  {
    sigmas.col(i) = observationFunc(sigmaPts.col(i));
    vec = vec + meanWts(i) * sigmas.col(i);
  }

  UnscentedKf::SigmaPointSet out {vec, sigmas};
  return out;
}

Eigen::MatrixXd UnscentedKf::fillMatrixWithVector(Eigen::VectorXd vec,
                                                  int numCols) const
{
  int numRows = vec.rows();
  Eigen::MatrixXd mat(numRows, numCols);
  for (int i = 0; i < numCols; i++)
  {
    mat.col(i) = vec;
  }
  return mat;
}
