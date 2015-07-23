
#include <iostream>
#include <fstream>
#include <vector>
#include <boost/iterator.hpp>
#include <pcl_ros/transforms.h>
#include <sensor_msgs/PointCloud2.h>
#include <std_msgs/Float32MultiArray.h>

#include "husky_trainer/Repeat.h"
#include "husky_trainer/ControllerMappings.h"

// Parameter names.
const std::string Repeat::SOURCE_TOPIC_PARAM = "readings_topic";
const std::string Repeat::COMMAND_OUTPUT_PARAM = "command_output_topic";
const std::string Repeat::WORKING_DIRECTORY_PARAM = "working_directory";

// Default values.
const std::string Repeat::DEFAULT_SOURCE_TOPIC = "/cloud";
const std::string Repeat::DEFAULT_COMMAND_OUTPUT_TOPIC = "/teach_repeat/desired_command";

const double Repeat::LOOP_RATE = 100.0;
const std::string Repeat::JOY_TOPIC = "/joy";
const std::string Repeat::REFERENCE_POSE_TOPIC = "/teach_repeat/reference_pose";
const std::string Repeat::ERROR_REPORTING_TOPIC = "/teach_repeat/raw_error";
const std::string Repeat::AP_SWITCH_TOPIC = "/teach_repeat/ap_switch";
const std::string Repeat::CLOUD_MATCHING_SERVICE = "/match_clouds";
const std::string Repeat::LIDAR_FRAME = "/velodyne";
const std::string Repeat::ROBOT_FRAME = "/base_link";
const std::string Repeat::WORLD_FRAME = "/odom";

Repeat::Repeat(ros::NodeHandle n) :
    loopRate(LOOP_RATE), controller(n)
{
    std::string workingDirectory;

    // Read parameters.
    n.param<std::string>(SOURCE_TOPIC_PARAM, sourceTopicName, DEFAULT_SOURCE_TOPIC);
    n.param<std::string>(WORKING_DIRECTORY_PARAM, workingDirectory, "");

    if(!chdir(workingDirectory.c_str()) != 0)
    {
        ROS_WARN("Failed to switch to the demanded directory.");
    }

    // Read from the teach files.
    loadCommands("speeds.sl", commands);
    loadPositions("positions.pl", positions);
    loadAnchorPoints("anchorPoints.apd", anchorPoints);
    ROS_INFO_STREAM("Done loading the teach in memory.");

    currentStatus = PAUSE;
    commandCursor = commands.begin();
    positionCursor = positions.begin();
    anchorPointCursor = anchorPoints.begin();

    // Make the appropriate subscriptions.
    readingTopic = n.subscribe(sourceTopicName, 10, &Repeat::cloudCallback, this);
    joystickTopic = n.subscribe(JOY_TOPIC, 1000, &Repeat::joystickCallback, this);
    errorReportingTopic = n.advertise<husky_trainer::TrajectoryError>(ERROR_REPORTING_TOPIC, 1000);
    commandRepeaterTopic = n.advertise<geometry_msgs::Twist>(DEFAULT_COMMAND_OUTPUT_TOPIC, 1000);
    referencePoseTopic = n.advertise<geometry_msgs::Pose>(REFERENCE_POSE_TOPIC, 100);
    anchorPointSwitchTopic = n.advertise<husky_trainer::AnchorPointSwitch>(AP_SWITCH_TOPIC, 1000);

    icpService = n.serviceClient<pointmatcher_ros::MatchClouds>(CLOUD_MATCHING_SERVICE, false);

    // Fetch the transform from lidar to base_link and cache it.
    tf::TransformListener tfListener;
    tfListener.waitForTransform(ROBOT_FRAME, LIDAR_FRAME, ros::Time(0), ros::Duration(5.0));
    tfListener.lookupTransform(ROBOT_FRAME, LIDAR_FRAME, ros::Time(0), tFromLidarToRobot);

    // Setup the dynamic reconfiguration server.
    dynamic_reconfigure::Server<husky_trainer::RepeatConfig>::CallbackType callback;
    callback = boost::bind(&Repeat::paramCallback, this, _1, _2);
    drServer.setCallback(callback);
}

void Repeat::spin()
{
    while(ros::ok() && commandCursor < commands.end() - 1)
    {
        ros::Time timeOfSpin = simTime();

        double distanceToCurrentAnchorPoint =
                geo_util::customDistance(poseOfTime(timeOfSpin), anchorPointCursor->getPosition());

        double distanceToNextAnchorPoint = (boost::next(anchorPointCursor) != anchorPoints.end()) ? 
                geo_util::customDistance(poseOfTime(timeOfSpin), boost::next(anchorPointCursor)->getPosition()) :
                std::numeric_limits<double>::infinity();

        //ROS_INFO("Distances. Current: %f, Next: %f", distanceToCurrentAnchorPoint, distanceToNextAnchorPoint);

        // Update the closest anchor point.
        if(boost::next(anchorPointCursor) < anchorPoints.end() &&
           distanceToCurrentAnchorPoint >= distanceToNextAnchorPoint)
        {
            anchorPointCursor++;

            husky_trainer::AnchorPointSwitch msg;
            msg.stamp = ros::Time::now();
            msg.newAnchorPoint = anchorPointCursor->name();
            anchorPointSwitchTopic.publish(msg);
        }

        //Update the command we are playing.
        if(currentStatus == PLAY)
        {
            geometry_msgs::Twist nextCommand = 
                controller.correctCommand(commandOfTime(timeOfSpin));
            commandRepeaterTopic.publish(nextCommand);
        }

        referencePoseTopic.publish(poseOfTime(simTime()));

        ros::spinOnce();
        loopRate.sleep();
    }
}

Repeat::~Repeat()
{
    readingTopic.shutdown();
    serviceCallLock.lock();
    serviceCallLock.unlock();
}

void Repeat::updateError(const sensor_msgs::PointCloud2& reading)
{
    tf::Transform tFromReadingToAnchor =
            geo_util::transFromPoseToPose(poseOfTime(simTime()), anchorPointCursor->getPosition());

    Eigen::Matrix4f eigenTransform;
    pcl_ros::transformAsMatrix(tFromReadingToAnchor*tFromLidarToRobot, eigenTransform);

    sensor_msgs::PointCloud2 transformedReadingCloudMsg;
    pcl_ros::transformPointCloud(eigenTransform, reading, transformedReadingCloudMsg);

    pointmatcher_ros::MatchClouds pmMessage;
    pmMessage.request.readings = transformedReadingCloudMsg;
    pmMessage.request.reference = anchorPointCursor->getCloud();

    if(serviceCallLock.try_lock())
    {
        if(icpService.call(pmMessage))
        {
            husky_trainer::TrajectoryError rawError = 
                pointmatching_tools::controlErrorOfTransformation(
                        pmMessage.response.transform
                        );
            controller.updateError(rawError);
            errorReportingTopic.publish(rawError);
        } else {
            ROS_WARN("There was a problem with the point matching service.");
            switchToStatus(ERROR);
        }
        serviceCallLock.unlock();
    } else {
        ROS_INFO("ICP service was busy, dropped a cloud.");
    }
}

void Repeat::cloudCallback(const sensor_msgs::PointCloud2ConstPtr msg)
{
    boost::thread thread(&Repeat::updateError, this, *msg);
}

void Repeat::joystickCallback(sensor_msgs::Joy::ConstPtr msg)
{
    switch(currentStatus)
    {
    case PLAY:
        if(msg->buttons[controller_mappings::RB] == 0) switchToStatus(PAUSE);
        break;
    case PAUSE:
        if(msg->buttons[controller_mappings::RB] == 1) switchToStatus(PLAY);
        break;
    case ERROR:
        if(msg->buttons[controller_mappings::X] == 1) switchToStatus(PAUSE);
        break;
    }
}

geometry_msgs::Twist Repeat::commandOfTime(ros::Time time)
{
    std::vector<geometry_msgs::TwistStamped>::iterator previousCursor = commandCursor;

    while(commandCursor->header.stamp < time + lookahead && 
            commandCursor < commands.end() - 1) 
    {
        previousCursor = commandCursor++;
    }

    return commandCursor->twist;
}

geometry_msgs::Pose Repeat::poseOfTime(ros::Time time)
{
    std::vector<geometry_msgs::PoseStamped>::iterator previousCursor = positionCursor;

    while(positionCursor->header.stamp < time && positionCursor < positions.end() - 1)
    {
        previousCursor = positionCursor++;
    }

    return positionCursor->pose;
}

void Repeat::pausePlayback()
{
    commandRepeaterTopic.publish(CommandRepeater::idleTwistCommand());
    baseSimTime += ros::Time::now() - timePlaybackStarted;
    ROS_INFO("Paused at: %lf", baseSimTime.toSec());
}

void Repeat::startPlayback()
{
    timePlaybackStarted = ros::Time::now();
}

ros::Time Repeat::simTime()
{
    if(currentStatus == PLAY) return baseSimTime + (ros::Time::now() - timePlaybackStarted);
    else return baseSimTime;

}

void Repeat::switchToStatus(Status desiredStatus)
{
    if(desiredStatus == ERROR && currentStatus != ERROR)
    {
        ROS_WARN("Switching to emergency mode.");
        pausePlayback();
        currentStatus = ERROR;
    }

    switch(currentStatus)
    {
    case PLAY:
        if(desiredStatus == PAUSE)
        {
            ROS_INFO("Stopping playback.");
            pausePlayback();
            currentStatus = PAUSE;
        }
        break;
    case PAUSE:
        if(desiredStatus == PLAY)
        {
            ROS_INFO("Starting playback.");
            startPlayback();
            currentStatus = PLAY;
        }
        break;
    case ERROR:
        if(desiredStatus == PAUSE)
        {
            ROS_INFO("Attempting recovery.");
            currentStatus = PAUSE;
        }
        break;
    }
}

void Repeat::loadCommands(const std::string filename, std::vector<geometry_msgs::TwistStamped>& out)
{
    std::ifstream commandFile(filename.c_str());
    out.clear();

    if(commandFile.is_open())
    {
        std::string lineBuffer;
        while(std::getline(commandFile, lineBuffer))
        {
            out.push_back(geo_util::stampedTwistOfString(lineBuffer));
        }

        commandFile.close();
    }
    else {
        ROS_ERROR_STREAM("Could not open command file: " << filename);
    }
}

void Repeat::loadPositions(const std::string filename, std::vector<geometry_msgs::PoseStamped>& out)
{
    std::ifstream positionFile(filename.c_str());
    out.clear();

    if(positionFile.is_open())
    {
        std::string lineBuffer;
        while(std::getline(positionFile, lineBuffer))
        {
            out.push_back(geo_util::stampedPoseOfString(lineBuffer));
        }

        positionFile.close();
    } else {
        ROS_ERROR_STREAM("Could not open position file: " << filename);
    }
}

void Repeat::loadAnchorPoints(const std::string filename, std::vector<AnchorPoint>& out)
{
    std::ifstream anchorPointsFile(filename.c_str());

    if(anchorPointsFile.is_open())
    {
        std::string lineBuffer;
        while(std::getline(anchorPointsFile, lineBuffer))
        {
            out.push_back(AnchorPoint(lineBuffer));
            out.back().loadFromDisk();
        }
        anchorPointsFile.close();
    } else {
        ROS_ERROR_STREAM("Could not open anchor points file: " << filename);
    }
}

void Repeat::paramCallback(husky_trainer::RepeatConfig& params, uint32_t level)
{
    lookahead = params.lookahead;
    controller.updateParams(params);
}
