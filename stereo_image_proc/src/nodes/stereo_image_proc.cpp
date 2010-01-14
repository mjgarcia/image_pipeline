/*********************************************************************
* Software License Agreement (BSD License)
* 
*  Copyright (c) 2008, Willow Garage, Inc.
*  All rights reserved.
* 
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
* 
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of the Willow Garage nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
* 
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*********************************************************************/

#include <ros/ros.h>
#include <ros/names.h>

#include <sensor_msgs/Image.h>
#include <sensor_msgs/CameraInfo.h>
#include <sensor_msgs/PointCloud.h>
#include <stereo_msgs/DisparityImage.h>
#include <sensor_msgs/fill_image.h>

#include <image_transport/image_transport.h>
#include <image_transport/subscriber_filter.h>
#include <message_filters/subscriber.h>
#include <message_filters/time_synchronizer.h>

#include <dynamic_reconfigure/server.h>
#include <stereo_image_proc/StereoImageProcConfig.h>

#include "stereo_image_proc/processor.h"

//
// Subscribes to two Image/CameraInfo topics, and performs rectification,
//   color processing, and stereo disparity on the images
//

class StereoProcNode
{
private:
  ros::NodeHandle nh_;
  image_transport::ImageTransport it_;
  image_transport::SubscriberFilter image_sub_l_, image_sub_r_;
  message_filters::Subscriber<sensor_msgs::CameraInfo> info_sub_l_, info_sub_r_;
  message_filters::TimeSynchronizer<sensor_msgs::Image, sensor_msgs::CameraInfo, 
				    sensor_msgs::Image, sensor_msgs::CameraInfo> sync_;

  std::string left_ns_, right_ns_;
  image_transport::Publisher pub_mono_l_;
  image_transport::Publisher pub_rect_l_;
  image_transport::Publisher pub_color_l_;
  image_transport::Publisher pub_rect_color_l_;
  image_transport::Publisher pub_mono_r_;
  image_transport::Publisher pub_rect_r_;
  image_transport::Publisher pub_color_r_;
  image_transport::Publisher pub_rect_color_r_;
  ros::Publisher pub_disparity_;
  ros::Publisher pub_pts_;

  // OK for these to be members in single-threaded case.
  stereo_image_proc::StereoProcessor processor_;
  image_geometry::StereoCameraModel model_;
  stereo_image_proc::StereoImageSet processed_images_;
  /// @todo Separate color_img_ to avoid some allocations?
  sensor_msgs::Image img_;
  int subscriber_count_;

public:

  StereoProcNode()
    : it_(nh_), sync_(3),
      subscriber_count_(0)
  {
    // Advertise outputs
    image_transport::SubscriberStatusCallback img_connect    = boost::bind(&StereoProcNode::connectCb, this);
    image_transport::SubscriberStatusCallback img_disconnect = boost::bind(&StereoProcNode::disconnectCb, this);
    left_ns_ = nh_.resolveName("left");
    pub_mono_l_       = it_.advertise(left_ns_+"/image_mono", 1, img_connect, img_disconnect);
    pub_rect_l_       = it_.advertise(left_ns_+"/image_rect", 1, img_connect, img_disconnect);
    pub_color_l_      = it_.advertise(left_ns_+"/image_color", 1, img_connect, img_disconnect);
    pub_rect_color_l_ = it_.advertise(left_ns_+"/image_rect_color", 1, img_connect, img_disconnect);

    right_ns_ = nh_.resolveName("right");
    pub_mono_r_       = it_.advertise(right_ns_+"/image_mono", 1, img_connect, img_disconnect);
    pub_rect_r_       = it_.advertise(right_ns_+"/image_rect", 1, img_connect, img_disconnect);
    pub_color_r_      = it_.advertise(right_ns_+"/image_color", 1, img_connect, img_disconnect);
    pub_rect_color_r_ = it_.advertise(right_ns_+"/image_rect_color", 1, img_connect, img_disconnect);

    ros::SubscriberStatusCallback msg_connect    = boost::bind(&StereoProcNode::connectCb, this);
    ros::SubscriberStatusCallback msg_disconnect = boost::bind(&StereoProcNode::disconnectCb, this);
    pub_disparity_ = nh_.advertise<stereo_msgs::DisparityImage>("disparity", 1, msg_connect, msg_disconnect);
    pub_pts_ = nh_.advertise<sensor_msgs::PointCloud>("points", 1, msg_connect, msg_disconnect);

    // Synchronize inputs. Topic subscriptions happen on demand in the connection callback.
    sync_.connectInput(image_sub_l_, info_sub_l_, image_sub_r_, info_sub_r_);
    sync_.registerCallback(boost::bind(&StereoProcNode::imageCb, this, _1, _2, _3, _4));
    
    /// @todo Print a warning every minute until the camera topics are advertised (like image_proc)
  }

  void connectCb()
  {
    if (subscriber_count_++ == 0) {
      ROS_DEBUG("Subscribing to camera topics");
      /// @todo Make left and right subscriptions independent. Not easily possible with current synch tools.
      image_sub_l_.subscribe(it_, left_ns_  + "/image_raw", 1);
      info_sub_l_ .subscribe(nh_, left_ns_  + "/camera_info", 1);
      image_sub_r_.subscribe(it_, right_ns_ + "/image_raw", 1);
      info_sub_r_ .subscribe(nh_, right_ns_ + "/camera_info", 1);
    }
  }

  void disconnectCb()
  {
    subscriber_count_--;
    if (subscriber_count_ == 0) {
      ROS_DEBUG("Unsubscribing from camera topics");
      image_sub_l_.unsubscribe();
      info_sub_l_ .unsubscribe();
      image_sub_r_.unsubscribe();
      info_sub_r_ .unsubscribe();
    }
  }

  void imageCb(const sensor_msgs::ImageConstPtr& raw_image_l, 
	       const sensor_msgs::CameraInfoConstPtr& cam_info_l,
	       const sensor_msgs::ImageConstPtr& raw_image_r, 
	       const sensor_msgs::CameraInfoConstPtr& cam_info_r)
  {
    // Update the camera model
    model_.fromCameraInfo(cam_info_l, cam_info_r);

    // Compute which outputs are in demand
    int flags = 0;
    typedef stereo_image_proc::StereoProcessor Proc;
    if (pub_mono_l_      .getNumSubscribers() > 0) flags |= Proc::LEFT_MONO;
    if (pub_rect_l_      .getNumSubscribers() > 0) flags |= Proc::LEFT_RECT;
    if (pub_color_l_     .getNumSubscribers() > 0) flags |= Proc::LEFT_COLOR;
    if (pub_rect_color_l_.getNumSubscribers() > 0) flags |= Proc::LEFT_RECT_COLOR;
    if (pub_mono_r_      .getNumSubscribers() > 0) flags |= Proc::RIGHT_MONO;
    if (pub_rect_r_      .getNumSubscribers() > 0) flags |= Proc::RIGHT_RECT;
    if (pub_color_r_     .getNumSubscribers() > 0) flags |= Proc::RIGHT_COLOR;
    if (pub_rect_color_r_.getNumSubscribers() > 0) flags |= Proc::RIGHT_RECT_COLOR;
    if (pub_disparity_   .getNumSubscribers() > 0) flags |= Proc::DISPARITY;
    if (pub_pts_         .getNumSubscribers() > 0) flags |= Proc::POINT_CLOUD;

    /// @todo Verify cameras are actually calibrated if rectification is needed

    // Process raw images into colorized/rectified/stereo outputs.
    if (!processor_.process(raw_image_l, raw_image_r, model_, processed_images_, flags))
      return;
    
    // Publish monocular output images
    img_.header = raw_image_l->header;
    // left
    if (flags & Proc::LEFT_MONO)
      publishImage(pub_mono_l_, processed_images_.left.mono, sensor_msgs::image_encodings::MONO8);
    if (flags & Proc::LEFT_RECT)
      publishImage(pub_rect_l_, processed_images_.left.rect, sensor_msgs::image_encodings::MONO8);
    if (flags & Proc::LEFT_COLOR)
      publishImage(pub_color_l_, processed_images_.left.color, processed_images_.left.color_encoding);
    if (flags & Proc::LEFT_RECT_COLOR)
      publishImage(pub_rect_color_l_, processed_images_.left.rect_color, processed_images_.left.color_encoding);
    // right
    if (flags & Proc::RIGHT_MONO)
      publishImage(pub_mono_r_, processed_images_.right.mono, sensor_msgs::image_encodings::MONO8);
    if (flags & Proc::RIGHT_RECT)
      publishImage(pub_rect_r_, processed_images_.right.rect, sensor_msgs::image_encodings::MONO8);
    if (flags & Proc::RIGHT_COLOR)
      publishImage(pub_color_r_, processed_images_.right.color, processed_images_.right.color_encoding);
    if (flags & Proc::RIGHT_RECT_COLOR)
      publishImage(pub_rect_color_r_, processed_images_.right.rect_color, processed_images_.right.color_encoding);

    // Publish stereo outputs
    if (flags & Proc::DISPARITY) {
      processed_images_.disparity.image.header = img_.header;
      processed_images_.disparity.header = img_.header;
      pub_disparity_.publish(processed_images_.disparity);
    }
    if (flags & Proc::POINT_CLOUD) {
      processed_images_.points.header = cam_info_l->header;
      pub_pts_.publish(processed_images_.points);
    }
  }
    

  void publishImage(const image_transport::Publisher& pub, const cv::Mat& image, const std::string& encoding)
  {
    fillImage(img_, encoding, image.rows, image.cols, image.step, const_cast<uint8_t*>(image.data));
    pub.publish(img_);
  }

  void configCallback(stereo_image_proc::StereoImageProcConfig &config, uint32_t level)
  {
    ROS_INFO("Reconfigure request received");

    processor_.setPreFilterSize(config.prefilter_size);
    processor_.setPreFilterCap(config.prefilter_cap);

    processor_.setCorrelationWindowSize(config.correlation_window_size);
    processor_.setMinDisparity(config.min_disparity);
    processor_.setDisparityRange(config.disparity_range);

    processor_.setUniquenessRatio(config.uniqueness_ratio);
    processor_.setTextureThreshold(config.texture_threshold);
    processor_.setSpeckleSize(config.speckle_size);
    processor_.setSpeckleRange(config.speckle_range);
  }
};

bool isRemapped(const std::string& name)
{
  if (ros::names::remap(name) != name) {
    ROS_WARN("[stereo_image_proc] Remapping '%s' no longer has any effect!", name.c_str());
    return true;
  }
  return false;
}

int main(int argc, char **argv)
{
  ros::init(argc, argv, "stereo_image_proc", ros::init_options::AnonymousName);
  if (isRemapped("camera") | isRemapped("camera_left") | isRemapped("camera_right") | isRemapped("output")) {
    ROS_WARN("stereo_image_proc should be started in the namespace of the camera.\nExample command-line usage:\n"
             "\t$ ROS_NAMESPACE=my_camera rosrun stereo_image_proc stereo_image_proc\n"
             "Or, for using two arbitrary cameras as a stereo pair (with 3d outputs in '/stereo'):\n"
             "\t$ ROS_NAMESPACE=stereo rosrun stereo_image_proc stereo_image_proc left:=/left_camera right:=/right_camera");
  }

  // Start stereo processor
  StereoProcNode proc;

  // Set up dynamic reconfiguration
  dynamic_reconfigure::Server<stereo_image_proc::StereoImageProcConfig> srv;
  dynamic_reconfigure::Server<stereo_image_proc::StereoImageProcConfig>::CallbackType f = 
    boost::bind(&StereoProcNode::configCallback, &proc, _1, _2);
  srv.setCallback(f);

  ros::spin();
  return 0;
}
