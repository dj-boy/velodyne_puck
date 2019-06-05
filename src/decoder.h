/*
 * This file is part of velodyne_puck driver.
 *
 * The driver is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * The driver is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with the driver.  If not, see <http://www.gnu.org/licenses/>.
 */
#pragma once

#include <ros/ros.h>

#include <image_transport/camera_publisher.h>
#include <image_transport/image_transport.h>
#include <pcl_ros/point_cloud.h>
#include <velodyne_puck/VelodynePacket.h>

#include "constants.h"

namespace velodyne_puck {

namespace it = image_transport;
using PointT = pcl::PointXYZI;
using CloudT = pcl::PointCloud<PointT>;

/// Convert image and camera_info to point cloud
CloudT::Ptr ToCloud(const sensor_msgs::ImageConstPtr& image_msg,
                    const sensor_msgs::CameraInfoConstPtr& cinfo_msg,
                    bool organized, float min_range, float max_range);

class Decoder {
 public:
  explicit Decoder(const ros::NodeHandle& pn);

  Decoder(const Decoder&) = delete;
  Decoder operator=(const Decoder&) = delete;

  using Ptr = boost::shared_ptr<Decoder>;
  using ConstPtr = boost::shared_ptr<const Decoder>;

  void PacketCb(const VelodynePacketConstPtr& packet_msg);

 private:
  /// ==========================================================================
  /// All of these uses laser index from velodyne which is interleaved
  /// 9.3.1.3 Data Point
  /// A data point is a measurement by one laser channel of a relection of a
  /// laser pulse
  struct DataPoint {
    uint16_t distance;
    uint8_t reflectivity;
  } __attribute__((packed));
  static_assert(sizeof(DataPoint) == 3, "sizeof(DataPoint) != 3");

  /// 9.3.1.1 Firing Sequence
  /// A firing sequence occurs when all the lasers in a sensor are fired. There
  /// are 16 firings per cycle for VLP-16
  struct FiringSequence {
    DataPoint points[kFiringsPerFiringSequence];  // 16
  } __attribute__((packed));
  static_assert(sizeof(FiringSequence) == 3 * 16,
                "sizeof(FiringSequence) != 48");

  /// 9.3.1.4 Azimuth
  /// A two-byte azimuth value (alpha) appears after the flag bytes at the
  /// beginning of each data block
  ///
  /// 9.3.1.5 Data Block
  /// The information from 2 firing sequences of 16 lasers is contained in each
  /// data block. Each packet contains the data from 24 firing sequences in 12
  /// data blocks.
  struct DataBlock {
    uint16_t flag;
    uint16_t azimuth;                                        // [0, 35999]
    FiringSequence sequences[kFiringSequencesPerDataBlock];  // 2
  } __attribute__((packed));
  static_assert(sizeof(DataBlock) == 100, "sizeof(DataBlock) != 100");

  struct Packet {
    DataBlock blocks[kDataBlocksPerPacket];  // 12
    /// The four-byte time stamp is a 32-bit unsigned integer marking the moment
    /// of the first data point in the first firing sequcne of the first data
    /// block
    uint32_t stamp;
    uint8_t factory[2];
  } __attribute__((packed));
  static_assert(sizeof(Packet) == sizeof(VelodynePacket().data),
                "sizeof(Packet) != 1206");

  /// Decoded result
  struct FiringSequenceStamped {
    double time;
    float azimuth;  // rad [0, 2pi)
    FiringSequence sequence;
  };

  // TODO: use vector or array?
  using Decoded = std::array<FiringSequenceStamped, kFiringSequencesPerPacket>;
  Decoded DecodePacket(const Packet* packet, double time) const;

  /// Convert firing sequences to image data
  sensor_msgs::ImagePtr ToImageData(
      const std::vector<FiringSequenceStamped>& tfseqs,
      sensor_msgs::CameraInfo& cinfo) const;

  /// Publish
  void PublishBufferAndClear();
  void PublishRange(const sensor_msgs::ImageConstPtr& image_msg);
  void PublishIntensity(const sensor_msgs::ImageConstPtr& image_msg);
  void PublishCloud(const sensor_msgs::ImageConstPtr& image_msg,
                    const sensor_msgs::CameraInfoConstPtr& cinfo_msg);

  // Configuration parameters
  double min_range_;
  double max_range_;
  bool organized_;

  // ROS related parameters
  std::string frame_id_;
  ros::NodeHandle pnh_;
  it::ImageTransport it_;
  ros::Subscriber packet_sub_;

  ros::Publisher cloud_pub_;
  it::Publisher intensity_pub_;
  it::Publisher range_pub_;
  it::CameraPublisher camera_pub_;

  std::vector<FiringSequenceStamped> buffer_;  // buffer
};

}  // namespace velodyne_puck