// ROS includes.
#include "ros/ros.h"
#include "ros/time.h"
#include <message_filters/subscriber.h>
#include <message_filters/synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>

#include <image_transport/image_transport.h>
#include <image_transport/subscriber_filter.h>
#include <sensor_msgs/CameraInfo.h>
#include <geometry_msgs/Pose.h>
#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/PoseArray.h>

#include <string.h>
#include <boost/thread.hpp>

#include <iostream>
#include <fstream>

#include <cv_bridge/cv_bridge.h>

#include "Matrix.h"
#include "Vector.h"
#include "Camera.h"
#include "pointcloud.h"
#include "detector.h"
#include "Globals.h"

#include "strands_perception_people_msgs/UpperBodyDetector.h"
#include "strands_perception_people_msgs/GroundPlane.h"

#include <QImage>
#include <QPainter>

#include <XmlRpcException.h>

using namespace std;
using namespace sensor_msgs;
using namespace message_filters;
using namespace strands_perception_people_msgs;

ros::Publisher pub_message, pub_centres, pub_closest;
image_transport::Publisher pub_result_image;

cv::Mat img_depth_;
cv_bridge::CvImagePtr cv_depth_ptr;	// cv_bridge for depth image

Matrix<double>* upper_body_template;
Detector* detector;


void render_bbox_2D(UpperBodyDetector& detections, QImage& image,
                    int r, int g, int b, int lineWidth)
{

    QPainter painter(&image);

    QColor qColor;
    qColor.setRgb(r, g, b);

    QPen pen;
    pen.setColor(qColor);
    pen.setWidth(lineWidth);

    painter.setPen(pen);

    for(int i = 0; i < detections.pos_x.size(); i++){
        int x =(int) detections.pos_x[i];
        int y =(int) detections.pos_y[i];
        int w =(int) detections.width[i];
        int h =(int) detections.height[i];

        painter.drawLine(x,y, x+w,y);
        painter.drawLine(x,y, x,y+h);
        painter.drawLine(x+w,y, x+w,y+h);
        painter.drawLine(x,y+h, x+w,y+h);
    }
}

bool checkParam(bool success, std::string param) {
    if(!success) {
        ROS_FATAL("Parameter: '%s' could not be found! Please make sure that the datacentre is running or start with 'with_datacentre:=false'", param.c_str());
    }
    return success;
}

bool ReadConfigParams(ros::NodeHandle n)
{
    bool success = true;

    std::string ns = ros::this_node::getName();
    ns += "/";

    //=====================================
    // Distance Range Accepted Detections
    //=====================================
    n.param(ns+"distance_range_accepted_detections", Globals::distance_range_accepted_detections, double(7.0));

    //======================================
    // ROI
    //======================================
    success = checkParam(n.getParam(ns+"inc_width_ratio", Globals::inc_width_ratio), ns+"inc_width_ratio") && success;
    ROS_INFO("ratio: %f",Globals::inc_width_ratio);
    success = checkParam(n.getParam(ns+"inc_height_ratio", Globals::inc_height_ratio), ns+"inc_height_ratio") && success;
    n.param(ns+"region_size_threshold", Globals::region_size_threshold, int(10));

    //======================================
    // Freespace Parameters
    //======================================
    n.param(ns+"freespace_scaleZ", Globals::freespace_scaleZ, double(20));
    n.param(ns+"freespace_scaleX", Globals::freespace_scaleX, double(20));
    n.param(ns+"freespace_minX", Globals::freespace_minX, double(-20));
    n.param(ns+"freespace_minZ", Globals::freespace_minZ, double(0));
    n.param(ns+"freespace_maxX", Globals::freespace_maxX, double(20));
    n.param(ns+"freespace_maxZ", Globals::freespace_maxZ, double(30));
    n.param(ns+"freespace_threshold", Globals::freespace_threshold, double(120));
    n.param(ns+"freespace_max_depth_to_cons", Globals::freespace_max_depth_to_cons, int(20));

    //======================================
    // Evaluation Parameters
    //======================================
    n.param(ns+"evaluation_NMS_threshold", Globals::evaluation_NMS_threshold, double(0.4));
    n.param(ns+"evaluation_NMS_threshold_LM", Globals::evaluation_NMS_threshold_LM, double(0.4));
    n.param(ns+"evaluation_NMS_threshold_Border", Globals::evaluation_NMS_threshold_Border, double(0.4));
    n.param(ns+"evaluation_inc_height_ratio", Globals::evaluation_inc_height_ratio, double(0.2));
    n.param(ns+"evaluation_stride", Globals::evaluation_stride, int(3));
    n.param(ns+"evaluation_scale_stride", Globals::evaluation_scale_stride, double(1.03));
    n.param(ns+"evaluation_nr_scales", Globals::evaluation_nr_scales, int(1));
    n.param(ns+"evaluation_inc_cropped_height", Globals::evaluation_inc_cropped_height, int(20));
    n.param(ns+"evaluation_greedy_NMS_overlap_threshold", Globals::evaluation_greedy_NMS_overlap_threshold, double(0.1));
    n.param(ns+"evaluation_greedy_NMS_threshold", Globals::evaluation_greedy_NMS_threshold, double(0.25));
    //======================================
    // World scale
    //======================================
    success = checkParam(n.getParam(ns+"WORLD_SCALE", Globals::WORLD_SCALE), ns+"WORLD_SCALE") && success;

    //======================================
    // height and width of images
    //======================================
    success = checkParam(n.getParam(ns+"dImHeight", Globals::dImHeight), ns+"dImHeight") && success;
    success = checkParam(n.getParam(ns+"dImWidth", Globals::dImWidth), ns+"dImWidth") && success;

    //====================================
    // Number of Frames / offset
    //====================================
    success = checkParam(n.getParam(ns+"numberFrames", Globals::numberFrames), ns+"numberFrames") && success;
    success = checkParam(n.getParam(ns+"nOffset", Globals::nOffset), ns+"nOffset") && success;

    //====================================
    // Size of Template
    //====================================
    success = checkParam(n.getParam(ns+"template_size", Globals::template_size), ns+"template_size") && success;

    n.param(ns+"max_height", Globals::max_height, double(2.0));
    n.param(ns+"min_height", Globals::min_height, double(1.4));

    return success;
}

void ReadUpperBodyTemplate(std::vector<double> up_temp, int x_size, int y_size)
{
    ROS_INFO("Reading template of size %d x %d = %d", x_size, y_size, x_size*y_size);
//    for(int i = 0; i < up_temp.size(); i++){
//        ROS_INFO("%d: %f", i, up_temp[i]);
//    }

    // read template from file
    upper_body_template = new Matrix<double>(x_size, y_size, &up_temp[0]);

    // resize it to the fixed size that is defined in Config File
    if(upper_body_template->x_size() > Globals::template_size)
    {
        upper_body_template->DownSample(Globals::template_size, Globals::template_size);
    }
    else if(upper_body_template->x_size() < Globals::template_size)
    {
        upper_body_template->UpSample(Globals::template_size, Globals::template_size);
    }
}

void callback(const ImageConstPtr &depth,  const ImageConstPtr &color,const GroundPlane::ConstPtr &gp, const CameraInfoConstPtr &info)
{
    // Check if calculation is necessary
    bool detect = pub_message.getNumSubscribers() > 0 || pub_centres.getNumSubscribers() > 0 || pub_closest.getNumSubscribers() > 0;
    bool vis = pub_result_image.getNumSubscribers() > 0;

    if(!detect && !vis)
        return;

    // Get depth image as matrix
    cv_depth_ptr = cv_bridge::toCvCopy(depth);
    img_depth_ = cv_depth_ptr->image;
    Matrix<double> matrix_depth(info->width, info->height);
    for (int r = 0;r < 480;r++){
        for (int c = 0;c < 640;c++) {
            matrix_depth(c, r) = img_depth_.at<float>(r,c);
        }
    }

    // Generate base camera
    Matrix<double> R = Eye<double>(3);
    Vector<double> t(3, 0.0);
    Matrix<double> K(3,3, (double*)&info->K[0]);

    // Get GP
    Vector<double> GP(3, (double*) &gp->n[0]);
    GP.pushBack((double) gp->d);

    // Detect upper bodies
    Camera camera(K,R,t,GP);
    PointCloud point_cloud(camera, matrix_depth);
    Vector<Vector< double > > detected_bounding_boxes;
    detector->ProcessFrame(camera, matrix_depth, point_cloud, *upper_body_template, detected_bounding_boxes);

    // Generate messages
    UpperBodyDetector detection_msg;
    detection_msg.header = depth->header;
    geometry_msgs::PoseArray bb_centres;
    bb_centres.header = depth->header;
    geometry_msgs::PoseStamped closest;
    closest.header = depth->header;
    closest.pose.position.z = 10000;
    closest.pose.orientation.w = 1;
    bool found = false;

    for(int i = 0; i < detected_bounding_boxes.getSize(); i++)
    {
        // Custom detections message
        detection_msg.pos_x.push_back(detected_bounding_boxes(i)(0));
        detection_msg.pos_y.push_back(detected_bounding_boxes(i)(1));
        detection_msg.width.push_back(detected_bounding_boxes(i)(2));
        detection_msg.height.push_back(detected_bounding_boxes(i)(3));
        detection_msg.dist.push_back(detected_bounding_boxes(i)(4));
        detection_msg.median_depth.push_back(detected_bounding_boxes(i)(5));

        // Calculate centres of bounding boxes
        double mid_point_x = detected_bounding_boxes(i)(0)+detected_bounding_boxes(i)(2)/2.0;
        double mid_point_y = detected_bounding_boxes(i)(1)+detected_bounding_boxes(i)(3)/2.0;

        // PoseArray message for boundingbox centres
        geometry_msgs::Pose pose;
        pose.position.x = detected_bounding_boxes(i)(5)*((mid_point_x-K(2,0))/K(0,0));
        pose.position.y = detected_bounding_boxes(i)(5)*((mid_point_y-K(2,1))/K(1,1));
        pose.position.z = detected_bounding_boxes(i)(5);
        pose.orientation.w = 1.0; //No rotation atm.
        bb_centres.poses.push_back(pose);
        if(closest.pose.position.z > pose.position.z) {
            closest.pose.position = pose.position;
            found = true;
        }
    }

    // Creating a ros image with the detection results an publishing it
    if(vis) {
        ROS_DEBUG("Publishing image");
        QImage image_rgb(&color->data[0], color->width, color->height, QImage::Format_RGB888); // would opencv be better?
        render_bbox_2D(detection_msg, image_rgb, 0, 0, 255, 2);

        sensor_msgs::Image sensor_image;
        sensor_image.header = color->header;
        sensor_image.height = image_rgb.height();
        sensor_image.width  = image_rgb.width();
        sensor_image.step   = color->step;
        vector<unsigned char> image_bits(image_rgb.bits(), image_rgb.bits()+sensor_image.height*sensor_image.width*3);
        sensor_image.data = image_bits;
        sensor_image.encoding = color->encoding;

        pub_result_image.publish(sensor_image);
    }

    // Publishing detections
    pub_message.publish(detection_msg);
    pub_centres.publish(bb_centres);
    if(found) pub_closest.publish(closest);
}

// Connection callback that unsubscribes from the tracker if no one is subscribed.
void connectCallback(message_filters::Subscriber<CameraInfo> &sub_cam,
                     message_filters::Subscriber<GroundPlane> &sub_gp,
                     image_transport::SubscriberFilter &sub_col,
                     image_transport::SubscriberFilter &sub_dep,
                     image_transport::ImageTransport &it){
    if(!pub_message.getNumSubscribers() && !pub_result_image.getNumSubscribers() && !pub_centres.getNumSubscribers()) {
        ROS_DEBUG("Upper Body Detector: No subscribers. Unsubscribing.");
        sub_cam.unsubscribe();
        sub_gp.unsubscribe();
        sub_col.unsubscribe();
        sub_dep.unsubscribe();
    } else {
        ROS_DEBUG("Upper Body Detector: New subscribers. Subscribing.");
        sub_cam.subscribe();
        sub_gp.subscribe();
        sub_col.subscribe(it,sub_col.getTopic().c_str(),1);
        sub_dep.subscribe(it,sub_dep.getTopic().c_str(),1);
    }
}

int main(int argc, char **argv)
{

    // Set up ROS.
    ros::init(argc, argv, "upper_body_detector");
    ros::NodeHandle n;

    // Declare variables that can be modified by launch file or command line.
    int queue_size;
    string cam_ns;
    string config_file;
    string template_path;
    string topic_gp;

    string pub_topic_centres;
    string pub_topic_closest;
    string pub_topic_ubd;
    string pub_topic_result_image;

    // Initialize node parameters from launch file or command line.
    // Use a private node handle so that multiple instances of the node can be run simultaneously
    // while using different parameters.
    ros::NodeHandle private_node_handle_("~");
    private_node_handle_.param("queue_size", queue_size, int(5));
    private_node_handle_.param("config_file", config_file, string(""));
    private_node_handle_.param("template_file", template_path, string(""));

    private_node_handle_.param("camera_namespace", cam_ns, string("/camera"));
    private_node_handle_.param("ground_plane", topic_gp, string("/ground_plane"));

    std::vector<double> up_temp;
    int x_size, y_size;
    private_node_handle_.getParam("x_size", x_size);
    private_node_handle_.getParam("y_size", y_size);
    if(!checkParam(private_node_handle_.getParam("upper_body_template", up_temp), ros::this_node::getName()+"/upper_body_template")){
        ROS_FATAL("No upper body template found");
        return 1;
    }

    string topic_depth_image = cam_ns + "/depth/image_rect_meters";
    string topic_color_image = cam_ns + "/rgb/image_rect_color";
    string topic_camera_info = cam_ns + "/depth/camera_info";

    // Printing queue size
    ROS_DEBUG("upper_body_detector: Queue size for synchronisation is set to: %i", queue_size);

    // Image transport handle
    image_transport::ImageTransport it(private_node_handle_);

    // Create a subscriber.
    // Set queue size to 1 because generating a queue here will only pile up images and delay the output by the amount of queued images
    image_transport::SubscriberFilter subscriber_depth;
    subscriber_depth.subscribe(it, topic_depth_image.c_str(),1); subscriber_depth.unsubscribe();
    message_filters::Subscriber<CameraInfo> subscriber_camera_info(n, topic_camera_info.c_str(), 1); subscriber_camera_info.unsubscribe();
    image_transport::SubscriberFilter subscriber_color;
    subscriber_color.subscribe(it, topic_color_image.c_str(), 1); subscriber_color.unsubscribe();
    message_filters::Subscriber<GroundPlane> subscriber_gp(n, topic_gp.c_str(), 1); subscriber_gp.unsubscribe();

    ros::SubscriberStatusCallback con_cb = boost::bind(&connectCallback,
                                                       boost::ref(subscriber_camera_info),
                                                       boost::ref(subscriber_gp),
                                                       boost::ref(subscriber_color),
                                                       boost::ref(subscriber_depth),
                                                       boost::ref(it));
    image_transport::SubscriberStatusCallback image_cb = boost::bind(&connectCallback,
                                                                     boost::ref(subscriber_camera_info),
                                                                     boost::ref(subscriber_gp),
                                                                     boost::ref(subscriber_color),
                                                                     boost::ref(subscriber_depth),
                                                                     boost::ref(it));

    //The real queue size for synchronisation is set here.
    sync_policies::ApproximateTime<Image, Image, GroundPlane, CameraInfo> MySyncPolicy(queue_size);
    MySyncPolicy.setAgePenalty(1000); //set high age penalty to publish older data faster even if it might not be correctly synchronized.

    // Initialise detector
    if(!ReadConfigParams(boost::ref(n))) return 1;
    ReadUpperBodyTemplate(up_temp, x_size, y_size);
    detector = new Detector();

    // Create synchronization policy. Here: async because time stamps will never match exactly
    const sync_policies::ApproximateTime<Image, Image, GroundPlane, CameraInfo> MyConstSyncPolicy = MySyncPolicy;
    Synchronizer< sync_policies::ApproximateTime<Image, Image, GroundPlane, CameraInfo> > sync(MyConstSyncPolicy,
                                                                                               subscriber_depth,
                                                                                               subscriber_color,
                                                                                               subscriber_gp,
                                                                                               subscriber_camera_info);
    // Register one callback for all topics
    sync.registerCallback(boost::bind(&callback, _1, _2, _3, _4));


    // Create publisher
    private_node_handle_.param("upper_body_detections", pub_topic_ubd, string("/upper_body_detector/detections"));
    pub_message = n.advertise<UpperBodyDetector>(pub_topic_ubd.c_str(), 10, con_cb, con_cb);

    private_node_handle_.param("upper_body_bb_centres", pub_topic_centres, string("/upper_body_detector/bounding_box_centres"));
    pub_centres = n.advertise<geometry_msgs::PoseArray>(pub_topic_centres.c_str(), 10, con_cb, con_cb);

    private_node_handle_.param("upper_body_closest_bb_centres", pub_topic_closest, string("/upper_body_detector/closest_bounding_box_centre"));
    pub_closest = n.advertise<geometry_msgs::PoseStamped>(pub_topic_closest.c_str(), 10, con_cb, con_cb);

    private_node_handle_.param("upper_body_image", pub_topic_result_image, string("/upper_body_detector/image"));
    pub_result_image = it.advertise(pub_topic_result_image.c_str(), 1, image_cb, image_cb);

    // Start ros thread managment
    ros::spin();

    return 0;
}

