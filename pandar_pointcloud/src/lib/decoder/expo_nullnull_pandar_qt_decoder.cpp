#include "pandar_pointcloud/decoder/expo_nullnull_pandar_qt_decoder.hpp"
#include "pandar_pointcloud/decoder/pandar_qt.hpp"

namespace
{
static inline double deg2rad(double degrees)
{
  return degrees * M_PI / 180.0;
}
}

namespace pandar_pointcloud
{
namespace pandar_qt
{
ExpoNullNullPandarQTDecoder::ExpoNullNullPandarQTDecoder(rclcpp::Node & node, Calibration& calibration, float scan_phase, float min_angle, float max_angle, double dual_return_distance_threshold, ReturnMode return_mode, RunMode run_mode, std::string background_map_path)
: logger_(node.get_logger()), clock_(node.get_clock())
{
  firing_offset_ = {
    12.31,  14.37,  16.43,  18.49,  20.54,  22.6,   24.66,  26.71,  29.16,  31.22,  33.28,  35.34,  37.39,
    39.45,  41.5,   43.56,  46.61,  48.67,  50.73,  52.78,  54.84,  56.9,   58.95,  61.01,  63.45,  65.52,
    67.58,  69.63,  71.69,  73.74,  75.8,   77.86,  80.9,   82.97,  85.02,  87.08,  89.14,  91.19,  93.25,
    95.3,   97.75,  99.82,  101.87, 103.93, 105.98, 108.04, 110.1,  112.15, 115.2,  117.26, 119.32, 121.38,
    123.43, 125.49, 127.54, 129.6,  132.05, 134.11, 136.17, 138.22, 140.28, 142.34, 144.39, 146.45,
  };

  for (size_t block = 0; block < BLOCK_NUM; ++block) {
    block_offset_single_[block] = 25.71f + 500.00f / 3.0f * block;
    block_offset_dual_[block] = 25.71f + 500.00f / 3.0f * (block / 2);
  }

  // TODO: add calibration data validation
  // if(calibration.elev_angle_map.size() != num_lasers_){
  //   // calibration data is not valid!
  // }
  for (size_t laser = 0; laser < UNIT_NUM; ++laser) {
    elev_angle_[laser] = calibration.elev_angle_map[laser];
    azimuth_offset_[laser] = calibration.azimuth_offset_map[laser];
  }

  scan_phase_ = static_cast<uint16_t>(scan_phase * 100.0f);
  min_angle_ = min_angle;
  max_angle_ = max_angle;
  return_mode_ = return_mode;
  run_mode_ = run_mode;
  background_map_path_ = background_map_path;
  dual_return_distance_threshold_ = dual_return_distance_threshold;

  last_phase_ = 0;
  has_scanned_ = false;

  scan_pc_.reset(new pcl::PointCloud<PointXYZIRADT>);
  overflow_pc_.reset(new pcl::PointCloud<PointXYZIRADT>);
  background_pc_.reset(new pcl::PointCloud<PointXYZIRADT>);
  background_overflow_pc_.reset(new pcl::PointCloud<PointXYZIRADT>);
  objects_pc_.reset(new pcl::PointCloud<PointXYZIRADT>);
  objects_overflow_pc_.reset(new pcl::PointCloud<PointXYZIRADT>);

  // Map stuff
  scan_counter_ = 0;
  memset(map_, 0, sizeof(map_));
  memset(num_map_samples_, 0, sizeof(num_map_samples_));

  if (run_mode_ == RunMode::SUBTRACT) {
    map_image_ = cv::imread(background_map_path_, cv::IMREAD_UNCHANGED);
  }
  else if (run_mode_ == RunMode::MAP) {
    map_image_ = cv::Mat::zeros(64, 600, CV_32FC1);
  }  
}

bool ExpoNullNullPandarQTDecoder::hasScanned()
{
  return has_scanned_;
}

PointcloudXYZIRADT ExpoNullNullPandarQTDecoder::getPointcloud()
{
  return scan_pc_;
}

PointcloudXYZIRADT ExpoNullNullPandarQTDecoder::getBackgroundPointcloud()
{
  return background_pc_;
}

PointcloudXYZIRADT ExpoNullNullPandarQTDecoder::getObjectsPointcloud()
{
  return objects_pc_;
}

void ExpoNullNullPandarQTDecoder::unpack(const pandar_msgs::msg::PandarPacket& raw_packet)
{
  if (!parsePacket(raw_packet)) {
    return;
  }

  if (has_scanned_) {
    scan_pc_ = overflow_pc_;
    overflow_pc_.reset(new pcl::PointCloud<PointXYZIRADT>);

    background_pc_ = background_overflow_pc_;
    background_overflow_pc_.reset(new pcl::PointCloud<PointXYZIRADT>);

    objects_pc_ = objects_overflow_pc_;
    objects_overflow_pc_.reset(new pcl::PointCloud<PointXYZIRADT>);
    has_scanned_ = false;
  }

  bool dual_return = (packet_.return_mode == DUAL_RETURN);
  auto step = dual_return ? 2 : 1;

  if (!dual_return) {
    if ((packet_.return_mode == FIRST_RETURN && return_mode_ != ReturnMode::FIRST) || 
        (packet_.return_mode == LAST_RETURN && return_mode_ != ReturnMode::LAST)) {
      RCLCPP_WARN(logger_, "Sensor return mode configuration does not match requested return mode");
    }
  }

  for (size_t block_id = 0; block_id < BLOCK_NUM; block_id += step) {
    int current_phase = (static_cast<int>(packet_.blocks[block_id].azimuth) - scan_phase_ + 36000) % 36000;
    if (current_phase > last_phase_ && !has_scanned_) {
      dual_return ? convert_dual(block_id, false) : convert(block_id, false);
      //*scan_pc_ += *block_pc;
    }
    else {
      dual_return ? convert_dual(block_id, true) : convert(block_id, true);
      //*overflow_pc_ += *block_pc;
      has_scanned_ = true;
      scan_counter_ ++;
      if (run_mode_ == RunMode::MAP && scan_counter_ == 40) {
        cv::imwrite(background_map_path_, map_image_);
      }
    }
    last_phase_ = current_phase;
  }
  return;
}

PointXYZIRADT ExpoNullNullPandarQTDecoder::build_point(int block_id, int unit_id, uint8_t return_type)
{
  const auto& block = packet_.blocks[block_id];
  const auto& unit = block.units[unit_id];
  double unix_second = static_cast<double>(timegm(&packet_.t));
  bool dual_return = (packet_.return_mode == DUAL_RETURN);
  PointXYZIRADT point;

  double xyDistance = unit.distance * cosf(deg2rad(elev_angle_[unit_id]));

  point.x = static_cast<float>(
      xyDistance * sinf(deg2rad(azimuth_offset_[unit_id] + (static_cast<double>(block.azimuth)) / 100.0)));
  point.y = static_cast<float>(
      xyDistance * cosf(deg2rad(azimuth_offset_[unit_id] + (static_cast<double>(block.azimuth)) / 100.0)));
  point.z = static_cast<float>(unit.distance * sinf(deg2rad(elev_angle_[unit_id])));

  point.intensity = unit.intensity;
  point.distance = unit.distance;
  point.ring = unit_id;
  point.azimuth = block.azimuth + round(azimuth_offset_[unit_id] * 100.0f);
  point.return_type = return_type;
  point.time_stamp = unix_second + (static_cast<double>(packet_.usec)) / 1000000.0;
  point.time_stamp += dual_return ? (static_cast<double>(block_offset_dual_[block_id] + firing_offset_[unit_id]) / 1000000.0f) :
                                    (static_cast<double>(block_offset_single_[block_id] + firing_offset_[unit_id]) / 1000000.0f); 

  if (run_mode_ == RunMode::MAP) {
    num_map_samples_[unit_id][block.azimuth/60] ++;
    if (map_[unit_id][block.azimuth/60] == 0.0) {
      map_[unit_id][block.azimuth/60] = unit.distance;
    }
    else {
      map_[unit_id][block.azimuth/60] -= map_[unit_id][block.azimuth/60] / num_map_samples_[unit_id][block.azimuth/60];
      map_[unit_id][block.azimuth/60] += unit.distance / num_map_samples_[unit_id][block.azimuth/60];
    }
    map_image_.row(unit_id).col(block.azimuth/60) = map_[unit_id][block.azimuth/60];
    //RCLCPP_WARN(logger_, "%d, %d", unit_id, block.azimuth/60);
  }

  return point;
}

void ExpoNullNullPandarQTDecoder::convert(const int block_id, bool overflow)
{
  //PointcloudXYZIRADT block_pc(new pcl::PointCloud<PointXYZIRADT>);

  const auto& block = packet_.blocks[block_id];

  for (size_t unit_id = 0; unit_id < UNIT_NUM; ++unit_id) {
    const auto& unit = block.units[unit_id];
    bool object = true;
    float corrected_azimuth = block.azimuth + round(azimuth_offset_[unit_id] * 100.0f);
    bool usable = true; //(unit.distance <= 0.2 || unit.distance > 15.0 || (corrected_azimuth < min_angle_ * 100.0f  && corrected_azimuth > max_angle_ * 100.0f)) ? 0 : 1;  
    if (min_angle_ > max_angle_) {
      usable = (unit.distance <= 0.2 || unit.distance > 15.0 || (corrected_azimuth < min_angle_ * 100.0f  && corrected_azimuth > max_angle_ * 100.0f)) ? 0 : 1;
    }
    else {
      usable = (unit.distance <= 0.2 || unit.distance > 15.0 || corrected_azimuth < min_angle_ * 100.0f || corrected_azimuth > max_angle_ * 100.0f) ? 0 : 1;
    }  
    if (run_mode_ == RunMode::SUBTRACT) {
      if (usable) {
        uint column = block.azimuth/60;
        uint row = (uint)unit_id;
        uint start_column = (column == 0) ? column : column - 1;
        uint end_column = (column == 599) ? column : column + 1;
        for (uint i = start_column; i <= end_column; i++) {
          if (abs(map_image_.at<float>(row, i) - unit.distance) < 0.2) {
            object = false;
          }
        }
      }
    }
    if (usable) {
      PointXYZIRADT new_point;
      new_point = build_point(block_id, unit_id, (packet_.return_mode == FIRST_RETURN) ? ReturnType::SINGLE_FIRST : ReturnType::SINGLE_LAST);
      if (overflow) {
        overflow_pc_->push_back(new_point);
        if (run_mode_ == RunMode::SUBTRACT) {
          object ? objects_overflow_pc_->push_back(new_point) : background_overflow_pc_->push_back(new_point);
        }
      }
      else {
        scan_pc_->push_back(new_point);
        if (run_mode_ == RunMode::SUBTRACT) {
          object ? objects_pc_->push_back(new_point) : background_pc_->push_back(new_point);
        }
      }
    }
  }
}

void ExpoNullNullPandarQTDecoder::convert_dual(const int block_id, bool overflow)
{
  //   Under the Dual Return mode, the ranging data from each firing is stored in two adjacent blocks:
  // · The even number block is the first return
  // · The odd number block is the last return
  // · The Azimuth changes every two blocks
  // · Important note: Hesai datasheet block numbering starts from 0, not 1, so odd/even are reversed here 
  //PointcloudXYZIRADT block_pc(new pcl::PointCloud<PointXYZIRADT>);

  int even_block_id = block_id;
  //int odd_block_id = block_id + 1;
  const auto& even_block = packet_.blocks[even_block_id];
  //const auto& odd_block = packet_.blocks[odd_block_id];

  for (size_t unit_id = 0; unit_id < UNIT_NUM; ++unit_id) {

    const auto& even_unit = even_block.units[unit_id];
    //const auto& odd_unit = odd_block.units[unit_id];

    bool even_usable = true;
    bool object = true;
    float corrected_azimuth = even_block.azimuth + round(azimuth_offset_[unit_id] * 100.0f);
    if (min_angle_ > max_angle_) {
      even_usable = (even_unit.distance <= 0.2 || even_unit.distance > 15.0 || (corrected_azimuth < min_angle_ * 100.0f  && corrected_azimuth > max_angle_ * 100.0f)) ? 0 : 1;
    }
    else {
      even_usable = (even_unit.distance <= 0.2 || even_unit.distance > 15.0 || corrected_azimuth < min_angle_ * 100.0f || corrected_azimuth > max_angle_ * 100.0f) ? 0 : 1;
    }
    if (run_mode_ == RunMode::SUBTRACT) {
      return_mode_ = ReturnMode::FIRST;
      if (even_usable) {
        uint column = even_block.azimuth/60;
        uint row = (uint)unit_id;
        uint start_column = (column == 0) ? column : column - 1;
        uint end_column = (column == 599) ? column : column + 1;
        for (uint i = start_column; i <= end_column; i++) {
          if (abs(map_image_.at<float>(row, i) - even_unit.distance) < 0.2) {
            object = false;
          }
        }
      }
    }
    // if ((ReturnMode::FIRST && even_usable) || (return_mode_ == ReturnMode::LAST && odd_usable)) {
      PointXYZIRADT new_point;
      if (return_mode_ == ReturnMode::FIRST && even_usable) {
        // First return is in even block
        new_point = build_point(even_block_id, unit_id, ReturnType::SINGLE_FIRST);
      }
      // else if (return_mode_ == ReturnMode::LAST && odd_usable) {
      //   new_point = build_point(odd_block_id, unit_id, ReturnType::SINGLE_LAST);
      // }
      if (overflow) {
        overflow_pc_->push_back(new_point);
        if (run_mode_ == RunMode::SUBTRACT) {
          object ? objects_overflow_pc_->push_back(new_point) : background_overflow_pc_->push_back(new_point);
        }
      }
      else {
        scan_pc_->push_back(new_point);
        if (run_mode_ == RunMode::SUBTRACT) {
          object ? objects_pc_->push_back(new_point) : background_pc_->push_back(new_point);
        }
      }
    // }
  }
}

bool ExpoNullNullPandarQTDecoder::parsePacket(const pandar_msgs::msg::PandarPacket& raw_packet)
{
  if (raw_packet.size != PACKET_SIZE && raw_packet.size != PACKET_WITHOUT_UDPSEQ_SIZE) {
    return false;
  }
  const uint8_t* buf = &raw_packet.data[0];

  size_t index = 0;
  // Parse 12 Bytes Header
  packet_.header.sob = (buf[index] & 0xff) << 8 | ((buf[index + 1] & 0xff));
  packet_.header.chProtocolMajor = buf[index + 2] & 0xff;
  packet_.header.chProtocolMinor = buf[index + 3] & 0xff;
  packet_.header.chLaserNumber = buf[index + 6] & 0xff;
  packet_.header.chBlockNumber = buf[index + 7] & 0xff;
  packet_.header.chReturnType = buf[index + 8] & 0xff;
  packet_.header.chDisUnit = buf[index + 9] & 0xff;
  index += HEAD_SIZE;

  if (packet_.header.sob != 0xEEFF) {
    // Error Start of Packet!
    return false;
  }

  for (size_t block = 0; block < static_cast<size_t>(packet_.header.chBlockNumber); block++) {
    packet_.blocks[block].azimuth = (buf[index] & 0xff) | ((buf[index + 1] & 0xff) << 8);
    index += BLOCK_HEADER_AZIMUTH;

    for (int unit = 0; unit < packet_.header.chLaserNumber; unit++) {
      unsigned int unRange = (buf[index] & 0xff) | ((buf[index + 1] & 0xff) << 8);

      packet_.blocks[block].units[unit].distance =
          (static_cast<double>(unRange * packet_.header.chDisUnit)) / (double)1000;
      packet_.blocks[block].units[unit].intensity = (buf[index + 2] & 0xff);
      packet_.blocks[block].units[unit].confidence = (buf[index + 3] & 0xff);
      index += UNIT_SIZE;
    }
  }

  index += RESERVED_SIZE;  // skip reserved bytes
  index += ENGINE_VELOCITY;

  packet_.usec = (buf[index] & 0xff) | (buf[index + 1] & 0xff) << 8 | ((buf[index + 2] & 0xff) << 16) |
                 ((buf[index + 3] & 0xff) << 24);
  index += TIMESTAMP_SIZE;

  packet_.return_mode = buf[index] & 0xff;

  index += RETURN_SIZE;
  index += FACTORY_SIZE;

  packet_.t.tm_year = (buf[index + 0] & 0xff) + 100;
  packet_.t.tm_mon = (buf[index + 1] & 0xff) - 1;
  packet_.t.tm_mday = buf[index + 2] & 0xff;
  packet_.t.tm_hour = buf[index + 3] & 0xff;
  packet_.t.tm_min = buf[index + 4] & 0xff;
  packet_.t.tm_sec = buf[index + 5] & 0xff;
  packet_.t.tm_isdst = 0;

  // in case of time error
  if (packet_.t.tm_year >= 200) {
    packet_.t.tm_year -= 100;
  }

  index += UTC_SIZE;

  return true;
}
}
}