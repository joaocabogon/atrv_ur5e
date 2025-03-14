// this is for emacs file handling -*- mode: c++; indent-tabs-mode: nil -*-

// -- BEGIN LICENSE BLOCK ----------------------------------------------
// Copyright 2019 FZI Forschungszentrum Informatik
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Many parts from this (Most of the URScript program) comes from the ur_modern_driver
// Copyright 2017, 2018 Simon Rasmussen (refactor)
// Copyright 2015, 2016 Thomas Timm Andersen (original version)
//
// -- END LICENSE BLOCK ------------------------------------------------

//----------------------------------------------------------------------
/*!\file
 *
 * \author  Felix Mauch mauch@fzi.de
 * \date    2019-04-11
 *
 */
//----------------------------------------------------------------------

#include "ur_rtde_driver/ur/ur_driver.h"
#include "ur_rtde_driver/exceptions.h"
#include "ur_rtde_driver/primary/primary_parser.h"
#include <memory>

#include <ur_rtde_driver/ur/calibration_checker.h>

namespace ur_driver
{
static const int32_t MULT_JOINTSTATE = 1000000;
static const std::string BEGIN_REPLACE("{{BEGIN_REPLACE}}");
static const std::string JOINT_STATE_REPLACE("{{JOINT_STATE_REPLACE}}");
static const std::string SERVO_J_REPLACE("{{SERVO_J_REPLACE}}");
static const std::string SERVER_IP_REPLACE("{{SERVER_IP_REPLACE}}");
static const std::string SERVER_PORT_REPLACE("{{SERVER_PORT_REPLACE}}");

ur_driver::UrDriver::UrDriver(const std::string& robot_ip, const std::string& script_file,
                              const std::string& output_recipe_file, const std::string& input_recipe_file,
                              std::function<void(bool)> handle_program_state,
                              std::unique_ptr<ToolCommSetup> tool_comm_setup, const std::string& calibration_checksum)
  : servoj_time_(0.008)
  , servoj_gain_(2000)
  , servoj_lookahead_time_(0.03)
  , reverse_interface_active_(false)
  , handle_program_state_(handle_program_state)
  , robot_ip_(robot_ip)
{
  LOG_DEBUG("Initializing urdriver");
  LOG_DEBUG("Initializing RTDE client");
  rtde_client_.reset(new rtde_interface::RTDEClient(robot_ip_, notifier_, output_recipe_file, input_recipe_file));

  primary_stream_.reset(new comm::URStream<ur_driver::primary_interface::PackageHeader>(
      robot_ip_, ur_driver::primary_interface::UR_PRIMARY_PORT));
  secondary_stream_.reset(new comm::URStream<ur_driver::primary_interface::PackageHeader>(
      robot_ip_, ur_driver::primary_interface::UR_SECONDARY_PORT));
  secondary_stream_->connect();
  LOG_INFO("Checking if calibration data matches connected robot.");
  checkCalibration(calibration_checksum);

  if (!rtde_client_->init())
  {
    throw UrException("Initialization of RTDE client went wrong.");
  }

  rtde_frequency_ = rtde_client_->getMaxFrequency();
  servoj_time_ = 1.0 / rtde_frequency_;

  std::string local_ip = rtde_client_->getIP();

  uint32_t reverse_port = 50001;        // TODO: Make this a parameter
  uint32_t script_sender_port = 50002;  // TODO: Make this a parameter

  std::string prog = readScriptFile(script_file);
  prog.replace(prog.find(JOINT_STATE_REPLACE), JOINT_STATE_REPLACE.length(), std::to_string(MULT_JOINTSTATE));
  std::ostringstream out;
  out << "lookahead_time=" << servoj_lookahead_time_ << ", gain=" << servoj_gain_;
  prog.replace(prog.find(SERVO_J_REPLACE), SERVO_J_REPLACE.length(), out.str());
  prog.replace(prog.find(SERVO_J_REPLACE), SERVO_J_REPLACE.length(), out.str());
  prog.replace(prog.find(SERVER_IP_REPLACE), SERVER_IP_REPLACE.length(), local_ip);
  prog.replace(prog.find(SERVER_PORT_REPLACE), SERVER_PORT_REPLACE.length(), std::to_string(reverse_port));

  auto urcontrol_version = rtde_client_->getVersion();

  std::stringstream begin_replace;
  if (tool_comm_setup != nullptr)
  {
    if (urcontrol_version.major < 5)
    {
      throw ToolCommNotAvailable("Tool communication setup requested, but this robot version does not support using "
                                 "the tool communication interface. Please check your configuration.",
                                 5, urcontrol_version.major);
    }
    begin_replace << "set_tool_voltage("
                  << static_cast<std::underlying_type<ToolVoltage>::type>(tool_comm_setup->getToolVoltage()) << ")\n";
    begin_replace << "set_tool_communication("
                  << "True"
                  << ", " << tool_comm_setup->getBaudRate() << ", "
                  << static_cast<std::underlying_type<Parity>::type>(tool_comm_setup->getParity()) << ", "
                  << tool_comm_setup->getStopBits() << ", " << tool_comm_setup->getRxIdleChars() << ", "
                  << tool_comm_setup->getTxIdleChars() << ")";
  }
  prog.replace(prog.find(BEGIN_REPLACE), BEGIN_REPLACE.length(), begin_replace.str());

  script_sender_.reset(new comm::ScriptSender(script_sender_port, prog));
  script_sender_->start();
  LOG_DEBUG("Created script sender");

  reverse_port_ = reverse_port;
  watchdog_thread_ = std::thread(&UrDriver::startWatchdog, this);

  rtde_client_->start();  // TODO: Add extra start method (also to HW-Interface)
  LOG_DEBUG("Initialization done");
}

std::unique_ptr<rtde_interface::DataPackage> ur_driver::UrDriver::getDataPackage()
{
  // TODO: This goes into the rtde_client
  std::unique_ptr<comm::URPackage<rtde_interface::PackageHeader>> urpackage;
  std::chrono::milliseconds timeout(100);  // We deliberately have a quite large timeout here, as the robot itself
                                           // should command the control loop's timing.
  if (rtde_client_->getDataPackage(urpackage, timeout))
  {
    rtde_interface::DataPackage* tmp = dynamic_cast<rtde_interface::DataPackage*>(urpackage.get());
    if (tmp != nullptr)
    {
      urpackage.release();
      return std::unique_ptr<rtde_interface::DataPackage>(tmp);
    }
  }
  return nullptr;
}

bool UrDriver::writeJointCommand(const vector6d_t& values)
{
  if (reverse_interface_active_)
  {
    return reverse_interface_->write(&values);
  }
  return false;
}

bool UrDriver::writeKeepalive()
{
  if (reverse_interface_active_)
  {
    vector6d_t* fake = nullptr;
    return reverse_interface_->write(fake, 1);
  }
  return false;
}

bool UrDriver::stopControl()
{
  if (reverse_interface_active_)
  {
    vector6d_t* fake = nullptr;
    return reverse_interface_->write(fake, 0);
  }
  return false;
}

void UrDriver::startWatchdog()
{
  handle_program_state_(false);
  reverse_interface_.reset(new comm::ReverseInterface(reverse_port_));
  reverse_interface_active_ = true;
  LOG_DEBUG("Created reverse interface");

  while (true)
  {
    LOG_INFO("Robot ready to receive control commands.");
    handle_program_state_(true);
    while (reverse_interface_active_ == true)
    {
      std::string keepalive = readKeepalive();

      if (keepalive == std::string(""))
      {
        reverse_interface_active_ = false;
      }
    }

    LOG_INFO("Connection to robot dropped, waiting for new connection.");
    handle_program_state_(false);
    reverse_interface_->~ReverseInterface();
    reverse_interface_.reset(new comm::ReverseInterface(reverse_port_));
    reverse_interface_active_ = true;
  }
}

std::string UrDriver::readScriptFile(const std::string& filename)
{
  std::ifstream ifs(filename);
  std::string content((std::istreambuf_iterator<char>(ifs)), (std::istreambuf_iterator<char>()));

  return content;
}
std::string UrDriver::readKeepalive()
{
  if (reverse_interface_active_)
  {
    return reverse_interface_->readKeepalive();
  }
  else
  {
    return std::string("");
  }
}

void UrDriver::checkCalibration(const std::string& checksum)
{
  if (primary_stream_ == nullptr)
  {
    throw std::runtime_error("checkCalibration() called without a primary interface connection being established.");
  }
  primary_interface::PrimaryParser parser;
  comm::URProducer<ur_driver::primary_interface::PackageHeader> prod(*primary_stream_, parser);

  CalibrationChecker consumer(checksum);

  comm::INotifier notifier;

  comm::Pipeline<ur_driver::primary_interface::PackageHeader> pipeline(prod, consumer, "Pipeline", notifier);
  pipeline.run();

  while (!consumer.isChecked())
  {
    ros::Duration(1).sleep();
  }
  ROS_DEBUG_STREAM("Got calibration information from robot.");
}

rtde_interface::RTDEWriter& UrDriver::getRTDEWriter()
{
  return rtde_client_->getWriter();
}

bool UrDriver::sendScript(const std::string& program)
{
  if (secondary_stream_ == nullptr)
  {
    throw std::runtime_error("Sending script to robot requested while there is no primary interface established. This "
                             "should not happen.");
  }

  size_t len = program.size();
  const uint8_t* data = reinterpret_cast<const uint8_t*>(program.c_str());
  size_t written;

  if (secondary_stream_->write(data, len, written))
  {
    LOG_DEBUG("Sent program to robot");
    return true;
  }
  LOG_ERROR("Could not send program to robot");
  return false;
}
}  // namespace ur_driver
