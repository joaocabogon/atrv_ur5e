// this is for emacs file handling -*- mode: c++; indent-tabs-mode: nil -*-

// -- BEGIN LICENSE BLOCK ----------------------------------------------
// -- END LICENSE BLOCK ------------------------------------------------

//----------------------------------------------------------------------
/*!\file
 *
 * \author  Felix Mauch mauch@fzi.de
 * \date    2019-06-14
 *
 */
//----------------------------------------------------------------------

#include <ur_rtde_driver/ur/calibration_checker.h>

namespace ur_driver
{
CalibrationChecker::CalibrationChecker(const std::string& expected_hash)
  : expected_hash_(expected_hash), checked_(false)
{
}
bool CalibrationChecker::consume(std::shared_ptr<comm::URPackage<primary_interface::PackageHeader>> product)
{
  auto kin_info = std::dynamic_pointer_cast<primary_interface::KinematicsInfo>(product);
  if (kin_info != nullptr)
  {
    // LOG_INFO("%s", product->toString().c_str());
    //
    if (kin_info->toHash() != expected_hash_)
    {
      LOG_ERROR("The calibration parameters of the connected robot don't match the ones from the given kinematics "
                "config file. Please be aware that this can lead to critical inaccuracies of tcp positions. Use the "
                "ur_calibration tool to extract the correct calibration from the robot and pass that into the "
                "description. See [TODO Link to documentation] for details.");
    }
    else
    {
      LOG_INFO("Calibration checked successfully.");
    }

    checked_ = true;
  }

  return true;
}
}  // namespace ur_driver
