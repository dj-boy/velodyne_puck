#include "constants.h"

#include <cv_bridge/cv_bridge.h>
#include <dynamic_reconfigure/server.h>
#include <image_transport/camera_publisher.h>
#include <image_transport/image_transport.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl_ros/point_cloud.h>

#include <sensor_msgs/CameraInfo.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/PointCloud2.h>
#include <velodyne_msgs/VelodynePacket.h>
#include <velodyne_msgs/VelodyneScan.h>
#include <velodyne_puck/VelodynePuckConfig.h>

namespace velodyne_puck {

using namespace sensor_msgs;
using namespace velodyne_msgs;

using Veckf = cv::Vec<float, 2>;
using PointT = pcl::PointXYZI;
using CloudT = pcl::PointCloud<PointT>;

class Decoder {
 public:
  /// Number of channels for image data
  static constexpr int kChannels = 2;  // (range [m], intensity)

  explicit Decoder(const ros::NodeHandle& pn);

  Decoder(const Decoder&) = delete;
  Decoder operator=(const Decoder&) = delete;

  void ScanCb(const velodyne_msgs::VelodyneScanConstPtr& scan_msg);
  void PacketCb(const velodyne_msgs::VelodynePacketConstPtr& packet_msg);
  void ConfigCb(VelodynePuckConfig& config, int level);

 private:
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
  static_assert(sizeof(FiringSequence) == 48, "sizeof(FiringSequence) != 48");

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
    /// block. The time stamp’s value is the number of microseconds elapsed
    /// since the top of the hour.
    uint32_t stamp;
    uint8_t factory[2];
  } __attribute__((packed));
  static_assert(sizeof(Packet) == sizeof(velodyne_msgs::VelodynePacket().data),
                "sizeof(Packet) != 1206");

  /// Decoded result
  struct FiringSequenceStamped {
    int64_t time;
    float azimuth;  // rad [0, 2pi)
    FiringSequence sequence;
  };

  // TODO: use vector or array?
  using Decoded = std::array<FiringSequenceStamped, kFiringSequencesPerPacket>;
  Decoded DecodePacket(const Packet* packet, int64_t time) const;

  /// Convert firing sequences to image data
  sensor_msgs::ImagePtr ToImage(
      const std::vector<FiringSequenceStamped>& tfseqs,
      sensor_msgs::CameraInfo& cinfo) const;

  /// Publish
  void PublishBufferAndClear();
  void PublishIntensity(const sensor_msgs::ImageConstPtr& image_msg);
  void PublishCloud(const sensor_msgs::ImageConstPtr& image_msg,
                    const sensor_msgs::CameraInfoConstPtr& cinfo_msg);

  // ROS related parameters
  std::string frame_id_;
  ros::NodeHandle pnh_;
  image_transport::ImageTransport it_;
  ros::Subscriber packet_sub_;
  ros::Publisher cloud_pub_;
  image_transport::Publisher intensity_pub_;
  image_transport::CameraPublisher camera_pub_;
  dynamic_reconfigure::Server<VelodynePuckConfig> cfg_server_;
  VelodynePuckConfig config_;
  std::vector<FiringSequenceStamped> buffer_;
};

/// Struct for precomputing sin and cos
struct SinCos {
  SinCos() = default;
  SinCos(float rad) : sin(std::sin(rad)), cos(std::cos(rad)) {}
  float sin, cos;
};

/// Convert image and camera_info to point cloud
CloudT::Ptr ToCloud(const ImageConstPtr& image_msg, const CameraInfo& cinfo_msg,
                    bool organized);

Decoder::Decoder(const ros::NodeHandle& pnh)
    : pnh_(pnh), it_(pnh), cfg_server_(pnh) {
  pnh_.param<std::string>("frame_id", frame_id_, "velodyne");
  ROS_INFO("Velodyne frame_id: %s", frame_id_.c_str());
  cfg_server_.setCallback(boost::bind(&Decoder::ConfigCb, this, _1, _2));
}

Decoder::Decoded Decoder::DecodePacket(const Packet* packet,
                                       int64_t time) const {
  // Azimuth is clockwise, which is absurd
  // ^ y
  // | a /
  // |--/
  // | /
  // |/
  // o ------- > x

  // Check return mode and product id, for now just die
  const auto return_mode = packet->factory[0];
  if (!(return_mode == 55 || return_mode == 56)) {
    ROS_ERROR(
        "return mode must be Strongest (55) or Last Return (56), "
        "instead got (%u)",
        return_mode);
    ros::shutdown();
  }
  const auto product_id = packet->factory[1];
  if (product_id != 34) {
    ROS_ERROR("product id must be VLP-16 or Puck Lite (34), instead got (%u)",
              product_id);
    ros::shutdown();
  }

  // std::array<TimedFiringSequence, kFiringSequencesPerPacket>;
  Decoded decoded;
  // For each data block, 12 total
  for (int dbi = 0; dbi < kDataBlocksPerPacket; ++dbi) {
    const auto& block = packet->blocks[dbi];
    const auto raw_azimuth = block.azimuth;
    ROS_WARN_STREAM_COND(raw_azimuth > kMaxRawAzimuth,
                         "Invalid raw azimuth: " << raw_azimuth);
    ROS_WARN_COND(block.flag != UPPER_BANK, "Invalid block %d", dbi);

    // Fill in decoded
    // for each firing sequence in the data block, 2
    for (int fsi = 0; fsi < kFiringSequencesPerDataBlock; ++fsi) {
      // Index into decoded, hardcode 2 for now
      const auto di = dbi * 2 + fsi;
      FiringSequenceStamped& tfseq = decoded[di];
      // Assume all firings within each firing sequence occur at the same time
      tfseq.time = time + di * kFiringCycleNs;
      tfseq.azimuth = Raw2Azimuth(block.azimuth);  // need to fix half later
      tfseq.sequence = block.sequences[fsi];
    }
  }

  // Fix azimuth for odd firing sequences
  for (int dbi = 0; dbi < kDataBlocksPerPacket; ++dbi) {
    // 1,3,5,...,23
    // Index into decoded with odd id, hardcode 2 for now
    const auto di = dbi * 2 + 1;
    auto azimuth = decoded[di].azimuth;

    auto prev = di - 1;
    auto next = di + 1;
    // Handle last block where there's no next to inerpolate
    // Just use the previous two
    if (dbi == kDataBlocksPerPacket - 1) {
      prev -= 2;
      next -= 2;
    }

    auto azimuth_prev = decoded[prev].azimuth;
    auto azimuth_next = decoded[next].azimuth;

    // Handle angle warping
    // Based on the fact that all raw azimuth is within 0 to 2pi
    if (azimuth_next < azimuth_prev) {
      azimuth_next += kTau;
    }

    ROS_WARN_COND(azimuth_prev > azimuth_next,
                  "azimuth_prev %f > azimuth_next %f", azimuth_prev,
                  azimuth_next);

    const auto azimuth_diff = (azimuth_next - azimuth_prev) / 2.0;

    azimuth += azimuth_diff;
    if (azimuth > kTau) {
      azimuth -= kTau;
    }

    decoded[di].azimuth = azimuth;
  }

  return decoded;
}

void Decoder::PacketCb(const VelodynePacketConstPtr& packet_msg) {
  const auto* my_packet =
      reinterpret_cast<const Packet*>(&(packet_msg->data[0]));

  // TODO: maybe make this a vector? to handle invalid data block?
  const auto decoded = DecodePacket(my_packet, packet_msg->stamp.toNSec());

  if (!config_.full_sweep) {
    for (const auto& tfseq : decoded) {
      buffer_.push_back(tfseq);
      if (buffer_.size() >= static_cast<size_t>(config_.image_width)) {
        ROS_DEBUG("Publish fixed width with buffer size: %zu, required: %d",
                  buffer_.size(), config_.image_width);
        PublishBufferAndClear();
      }
    }
  } else {
    // Full scan mode 0~360
    // Check for a change of azimuth across 0
    float prev_azimuth = buffer_.empty() ? -1 : buffer_.back().azimuth;

    for (const auto& tfseq : decoded) {
      if (tfseq.azimuth < prev_azimuth) {
        // this indicates we cross the 0 azimuth angle
        // we are ready to publish what's in the buffer
        ROS_DEBUG("curr_azimuth: %f < %f prev_azimuth", rad2deg(tfseq.azimuth),
                  rad2deg(prev_azimuth));
        ROS_DEBUG("Publish full scan with buffer size: %zu", buffer_.size());
        PublishBufferAndClear();
      }

      buffer_.push_back(tfseq);
      prev_azimuth = tfseq.azimuth;
    }
  }
}

void Decoder::ScanCb(const VelodyneScanConstPtr& scan_msg) {
  // Not implemented at the moment
}

void Decoder::ConfigCb(VelodynePuckConfig& config, int level) {
  if (config.min_range > config.max_range) {
    ROS_WARN("min_range: %f > max_range: %f", config.min_range,
             config.max_range);
    config.min_range = config.max_range;
  }

  ROS_INFO(
      "Reconfigure Request: min_range: %f, max_range: %f, image_width: %d, "
      "organized: %s, full_sweep: %s",
      config.min_range, config.max_range, config.image_width,
      config.organized ? "True" : "False",
      config.full_sweep ? "True" : "False");

  config_ = config;
  buffer_.clear();

  if (level == -1) {
    ROS_INFO("Initialize ROS subscriber/publisher");
    cloud_pub_ = pnh_.advertise<PointCloud2>("cloud", 10);
    intensity_pub_ = it_.advertise("intensity", 1);
    camera_pub_ = it_.advertiseCamera("image", 5);

    packet_sub_ =
        pnh_.subscribe<VelodynePacket>("packet", 100, &Decoder::PacketCb, this);
    ROS_INFO("Ready to publish");
  }
}

void Decoder::PublishBufferAndClear() {
  if (buffer_.empty()) return;

  const auto start = ros::Time::now();
  // Always convert to image data
  const CameraInfoPtr cinfo_msg(new CameraInfo);
  const auto image_msg = ToImage(buffer_, *cinfo_msg);

  if (camera_pub_.getNumSubscribers()) {
    camera_pub_.publish(image_msg, cinfo_msg);
  }

  if (intensity_pub_.getNumSubscribers()) {
    PublishIntensity(image_msg);
  }

  if (cloud_pub_.getNumSubscribers()) {
    PublishCloud(image_msg, cinfo_msg);
  }

  ROS_DEBUG("Clearing buffer %zu", buffer_.size());
  buffer_.clear();

  const auto time = (ros::Time::now() - start).toSec();
  ROS_DEBUG("Total time for publish: %f", time);
}

void Decoder::PublishIntensity(const ImageConstPtr& image_msg) {
  const auto image = cv_bridge::toCvShare(image_msg)->image;
  cv::Mat intensity;
  cv::extractChannel(image, intensity, 1);
  intensity.convertTo(intensity, CV_8UC1);
  intensity_pub_.publish(
      cv_bridge::CvImage(image_msg->header, "mono8", intensity).toImageMsg());
}

void Decoder::PublishCloud(const ImageConstPtr& image_msg,
                           const CameraInfoConstPtr& cinfo_msg) {
  const auto cloud = ToCloud(image_msg, *cinfo_msg, config_.organized);
  ROS_DEBUG("number of points in cloud: %zu", cloud->size());
  cloud_pub_.publish(cloud);
}

ImagePtr Decoder::ToImage(const std::vector<FiringSequenceStamped>& fseqs,
                          CameraInfo& cinfo_msg) const {
  std_msgs::Header header;
  header.stamp.fromNSec(fseqs.front().time);
  header.frame_id = frame_id_;

  cv::Mat image = cv::Mat::zeros(kFiringsPerFiringSequence, fseqs.size(),
                                 CV_32FC(kChannels));
  ROS_DEBUG("image: %d x %d x %d", image.rows, image.cols, image.channels());

  cinfo_msg.header = header;
  cinfo_msg.height = image.rows;
  cinfo_msg.width = image.cols;
  cinfo_msg.K[0] = kMinElevation;
  cinfo_msg.K[1] = kMaxElevation;
  cinfo_msg.R[0] = kDistanceResolution;
  cinfo_msg.P[0] = kFiringCycleNs;   // ns
  cinfo_msg.P[1] = kSingleFiringNs;  // ns
  cinfo_msg.distortion_model = "VLP16";
  cinfo_msg.D.reserve(image.cols);

  // Unfortunately the buffer element is organized in columns, probably not very
  // cache-friendly
  for (int c = 0; c < image.cols; ++c) {
    const auto& tfseq = fseqs[c];
    // D stores each azimuth angle
    cinfo_msg.D.push_back(tfseq.azimuth);

    // Fill in image
    for (int r = 0; r < image.rows; ++r) {
      // row 0 corresponds to max elevation (highest), row 15 corresponds to
      // min elevation (lowest) hence we flip row number
      // also data points are stored in laser ids which are interleaved, so we
      // need to convert to index first. See p54 table
      const auto rr = Index2LaserId(kFiringsPerFiringSequence - 1 - r);

      // We clip range in image instead of in cloud
      float range = tfseq.sequence.points[rr].distance * kDistanceResolution;
      if (range < config_.min_range || range > config_.max_range) {
        range = kNaNFloat;
      }

      auto& e = image.at<Veckf>(r, c);
      e[0] = range;                                   // range
      e[1] = tfseq.sequence.points[rr].reflectivity;  // intensity
    }
  }

  return cv_bridge::CvImage(header, "32FC" + std::to_string(kChannels), image)
      .toImageMsg();
}

CloudT::Ptr ToCloud(const ImageConstPtr& image_msg, const CameraInfo& cinfo_msg,
                    bool organized) {
  CloudT::Ptr cloud_ptr(new CloudT);
  CloudT& cloud = *cloud_ptr;

  const auto image = cv_bridge::toCvShare(image_msg)->image;
  const auto& azimuths = cinfo_msg.D;

  const float min_elevation = cinfo_msg.K[0];
  const float max_elevation = cinfo_msg.K[1];
  const float delta_elevation =
      (max_elevation - min_elevation) / (image.rows - 1);

  // Precompute sin cos
  std::vector<SinCos> sincos;
  sincos.reserve(azimuths.size());
  for (const auto& a : azimuths) {
    sincos.emplace_back(a);
  }

  cloud.header = pcl_conversions::toPCL(image_msg->header);
  cloud.reserve(image.total());

  for (int r = 0; r < image.rows; ++r) {
    const auto* const row_ptr = image.ptr<Veckf>(r);
    // Because image row 0 is the highest laser point
    const float omega = max_elevation - r * delta_elevation;
    const auto cos_omega = std::cos(omega);
    const auto sin_omega = std::sin(omega);

    for (int c = 0; c < image.cols; ++c) {
      const Veckf& data = row_ptr[c];

      PointT p;
      if (std::isnan(data[0])) {
        if (organized) {
          p.x = p.y = p.z = kNaNFloat;
          cloud.points.push_back(p);
        }
      } else {
        // p.53 Figure 9-1 VLP-16 Sensor Coordinate System
        // x = d * cos(w) * sin(a);
        // y = d * cos(w) * cos(a);
        // z = d * sin(w)
        const float R = data[0];
        const auto x = R * cos_omega * sincos[c].sin;
        const auto y = R * cos_omega * sincos[c].cos;
        const auto z = R * sin_omega;

        // original velodyne frame is x right y forward
        // we make x forward and y left, thus 0 azimuth is at x = 0 and
        // goes clockwise
        p.x = y;
        p.y = -x;
        p.z = z;
        p.intensity = data[1];

        cloud.points.push_back(p);
      }
    }
  }

  if (organized) {
    cloud.width = image.cols;
    cloud.height = image.rows;
  } else {
    cloud.width = cloud.size();
    cloud.height = 1;
  }

  return cloud_ptr;
}

}  // namespace velodyne_puck

int main(int argc, char** argv) {
  ros::init(argc, argv, "velodyne_puck_decoder");
  ros::NodeHandle pnh("~");

  velodyne_puck::Decoder node(pnh);
  ros::spin();
}
