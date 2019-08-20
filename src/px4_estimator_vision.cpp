/***************************************************************************************************************************
 * px4_estimator_vision.cpp
 *
 * Author: Longhao Qian
 *
 * Update Time: 2019.8.20
 *
 * 说明: mavros位置估计程序 (optitrack only)
 *      1. subscribe position and velocity data of drone and payload from ground station via Mocap topic
 *      3. 订阅飞控发布的位置、速度及欧拉角信息，作对比用
 *      4. 存储飞行数据，实验分析及作图使用
 *      5. 选择激光SLAM或者Mocap设备作为位置来源，发布位置及偏航角(xyz+yaw)给飞控
 *
***************************************************************************************************************************/
#include <ros/ros.h>

#include <iostream>
#include <Eigen/Eigen>
#include <state_from_mavros.h>
#include <OptiTrackFeedBackRigidBody.h>
#include <math_utils.h>
#include <Frame_tf_utils.h>
//msg 头文件
#include <mavros_msgs/CommandBool.h>
#include <mavros_msgs/SetMode.h>
#include <mavros_msgs/State.h>
#include <geometry_msgs/Vector3.h>
#include <geometry_msgs/TwistStamped.h>
#include <geometry_msgs/PoseStamped.h>
#include <nav_msgs/Odometry.h>
#include <sensor_msgs/Imu.h>
#include <std_msgs/Bool.h>
#include <geometry_msgs/Pose.h>
#include <geometry_msgs/Vector3Stamped.h>
#include <geometry_msgs/Point.h>
#include <std_msgs/UInt16.h>
#include <std_msgs/Float64.h>
#include <tf2_msgs/TFMessage.h>
#include <geometry_msgs/TransformStamped.h>
#include <sensor_msgs/Range.h>
#include <px4_command/DroneState.h>
#include <px4_command/Mocap.h>
#include <LowPassFilter.h>

using namespace std;
//---------------------------------------相关参数-----------------------------------------------
int flag_use_laser_or_vicon;                               //0:使用mocap数据作为定位数据 1:使用laser数据作为定位数据
float Use_mocap_raw;
int linear_window;
int angular_window;
float noise_a,noise_b;
float noise_T;
//rigidbody_state UAVstate;
//rigidbody_state Payloadstate;
//---------------------------------------vicon定位相关------------------------------------------
Eigen::Vector3d pos_drone_mocap;                          //无人机当前位置 (vicon)
Eigen::Quaterniond q_mocap;
Eigen::Vector3d Euler_mocap;                              //无人机当前姿态 (vicon)
//---------------------------------------laser定位相关------------------------------------------
Eigen::Vector3d pos_drone_laser;                          //无人机当前位置 (laser)
Eigen::Quaterniond q_laser;
Eigen::Vector3d Euler_laser;                                         //无人机当前姿态(laser)

geometry_msgs::TransformStamped laser;                          //当前时刻cartorgrapher发布的数据
geometry_msgs::TransformStamped laser_last;
//---------------------------------------无人机位置及速度--------------------------------------------
Eigen::Vector3d pos_drone_fcu;                           //无人机当前位置 (来自fcu)
Eigen::Vector3d vel_drone_fcu;                           //无人机上一时刻位置 (来自fcu)
Eigen::Vector3d Att_fcu;                               //无人机当前欧拉角(来自fcu)
Eigen::Vector3d Att_rate_fcu;
//---------------------------------------发布相关变量--------------------------------------------
ros::Publisher vision_pub;
ros::Publisher drone_state_pub;
px4_command::DroneState _DroneState;  
px4_command::Mocap UAV_motion;
px4_command::Mocap Payload_motion;
bool UAVsubFlag;
bool PaylaodsubFlag;
bool MocapOK;
//>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>函数声明<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
void printf_info();                                                                       //打印函数
void send_to_fcu();
void publish_drone_state();
void printf_param();
//>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>回调函数<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
void UAV_sb(const px4_command::Mocap::ConstPtr& msg)
{
    UAV_motion = *msg;
    UAVsubFlag = true;
}
void Payload_sb(const px4_command::Mocap::ConstPtr& msg)
{
    Payload_motion = *msg;
    PaylaodsubFlag = true;
}
void laser_cb(const tf2_msgs::TFMessage::ConstPtr& msg)
{
    //确定是cartographer发出来的/tf信息
    //有的时候/tf这个消息的发布者不止一个
    if (msg->transforms[0].header.frame_id == "map")
    {
        laser = msg->transforms[0];

        float dt_laser;

        dt_laser = (laser.header.stamp.sec - laser_last.header.stamp.sec) + (laser.header.stamp.nsec - laser_last.header.stamp.nsec)/10e9;

        //这里需要做这个判断是因为cartographer发布位置时有一个小bug，ENU到NED不展开讲。
        if (dt_laser != 0)
        {
            //位置 xy  [将解算的位置从laser坐标系转换至ENU坐标系]???
            pos_drone_laser[0]  = laser.transform.translation.x;
            pos_drone_laser[1]  = laser.transform.translation.y;

            // Read the Quaternion from the Carto Package [Frame: Laser[ENU]]
            Eigen::Quaterniond q_laser_enu(laser.transform.rotation.w, laser.transform.rotation.x, laser.transform.rotation.y, laser.transform.rotation.z);

            q_laser = q_laser_enu;

            // Transform the Quaternion to Euler Angles
            Euler_laser = quaternion_to_euler(q_laser);
        }

        laser_last = laser;
    }
}
void sonic_cb(const std_msgs::UInt16::ConstPtr& msg)
{
    std_msgs::UInt16 sonic;

    sonic = *msg;

    //位置
    pos_drone_laser[2]  = (float)sonic.data / 1000;
}

void tfmini_cb(const sensor_msgs::Range::ConstPtr& msg)
{
    sensor_msgs::Range tfmini;

    tfmini = *msg;

    //位置
    pos_drone_laser[2]  = tfmini.range ;

}

void optitrack_cb(const geometry_msgs::PoseStamped::ConstPtr& msg)
{
    //位置 -- optitrack系 到 ENU系
    //Frame convention 0: Z-up -- 1: Y-up (See the configuration in the motive software)
    int optitrack_frame = 0; 
    if( optitrack_frame == 0) {
        // Read the Drone Position from the Vrpn Package [Frame: Vicon]  (Vicon to ENU frame)
        pos_drone_mocap = Eigen::Vector3d(msg->pose.position.x,msg->pose.position.y,msg->pose.position.z);
        // Read the Quaternion from the Vrpn Package [Frame: Vicon[ENU]]
        q_mocap = Eigen::Quaterniond(msg->pose.orientation.w, msg->pose.orientation.x, msg->pose.orientation.y, msg->pose.orientation.z);
    }
    else {
        // Read the Drone Position from the Vrpn Package [Frame: Vicon]  (Vicon to ENU frame)
        pos_drone_mocap = Eigen::Vector3d(-msg->pose.position.x,msg->pose.position.z,msg->pose.position.y);
        // Read the Quaternion from the Vrpn Package [Frame: Vicon[ENU]]
        q_mocap = Eigen::Quaterniond(msg->pose.orientation.w, msg->pose.orientation.x, msg->pose.orientation.z, msg->pose.orientation.y); //Y-up convention, switch the q2 & q3
    }

    // Transform the Quaternion to Euler Angles
    Euler_mocap = quaternion_to_euler(q_mocap);

}

//>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>主 函 数<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
int main(int argc, char **argv)
{
    ros::init(argc, argv, "px4_pos_estimator");
    ros::NodeHandle nh("~");

    //读取参数表中的参数
    // 使用激光SLAM数据orVicon数据 0 for vicon， 1 for 激光SLAM
    nh.param<int>("pos_estimator/flag_use_laser_or_vicon", flag_use_laser_or_vicon, 0);

    // 0 for use the data from fcu, 1 for use the mocap raw data(only position), 2 for use the mocap raw data(position and velocity)
    nh.param<float>("pos_estimator/Use_mocap_raw", Use_mocap_raw, 0.0);

    // window for linear velocity
    nh.param<int>("pos_estimator/linear_window", linear_window, 3);

    // window for linear velocity
    nh.param<int>("pos_estimator/angular_window", angular_window, 3);

    nh.param<float>("pos_estimator/noise_a", noise_a, 0.0);

    nh.param<float>("pos_estimator/noise_b", noise_b, 0.0);

    nh.param<float>("pos_estimator/noise_T", noise_T, 0.5);

    printf_param();



    LowPassFilter LPF_x;
    LowPassFilter LPF_y;
    LowPassFilter LPF_z;

    LPF_x.set_Time_constant(noise_T);
    LPF_y.set_Time_constant(noise_T);
    LPF_z.set_Time_constant(noise_T);


    // 【订阅】cartographer估计位置
    ros::Subscriber laser_sub = nh.subscribe<tf2_msgs::TFMessage>("/tf", 1000, laser_cb);

    // 【订阅】超声波的数据
    ros::Subscriber sonic_sub = nh.subscribe<std_msgs::UInt16>("/sonic", 100, sonic_cb);

    // 【订阅】tf mini的数据
    ros::Subscriber tfmini_sub = nh.subscribe<sensor_msgs::Range>("/TFmini/TFmini", 100, tfmini_cb);

    // 【订阅】optitrack估计位置
    ros::Subscriber optitrack_sub = nh.subscribe<geometry_msgs::PoseStamped>("/vrpn_client_node/UAV/pose", 1000, optitrack_cb);

    ros::Subscriber UAV_motion_sub = nh.subscribe<px4_command::Mocap>("/px4_command/UAV", 1000, UAV_sb);
    ros::Subscriber Payload_motion_sub = nh.subscribe<px4_command::Mocap>("/px4_command/Payload", 1000, Payload_sb);

    // 【发布】无人机位置和偏航角 坐标系 ENU系
    //  本话题要发送飞控(通过mavros_extras/src/plugins/vision_pose_estimate.cpp发送), 对应Mavlink消息为VISION_POSITION_ESTIMATE(#??), 对应的飞控中的uORB消息为vehicle_vision_position.msg 及 vehicle_vision_attitude.msg
    vision_pub = nh.advertise<geometry_msgs::PoseStamped>("/mavros/vision_pose/pose", 100);

    drone_state_pub = nh.advertise<px4_command::DroneState>("/px4_command/drone_state", 10);

    // 用于与mavros通讯的类，通过mavros接收来至飞控的消息【飞控->mavros->本程序】
    state_from_mavros _state_from_mavros;

    // 频率
    ros::Rate rate(60.0);

    MocapOK = false;

//>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>Main Loop<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
    while(ros::ok())
    {
        //回调一次 更新传感器状态
        ros::spinOnce();

        // 将定位信息及偏航角信息发送至飞控，根据参数flag_use_laser_or_vicon选择定位信息来源
        send_to_fcu();

        for (int i=0;i<3;i++)
        {
            pos_drone_fcu[i] = _state_from_mavros._DroneState.position[i];                           
            vel_drone_fcu[i] = _state_from_mavros._DroneState.velocity[i];                           
            Att_fcu[i] = _state_from_mavros._DroneState.attitude[i];  
            Att_rate_fcu[i] = _state_from_mavros._DroneState.attitude_rate[i];  
        }

        // 发布无人机状态至px4_pos_controller.cpp节点，根据参数Use_mocap_raw选择位置速度消息来源
        // get drone state from _state_from_mavros
        _DroneState = _state_from_mavros._DroneState;
        _DroneState.header.stamp = ros::Time::now();

        Eigen::Vector3d random;

        for (int i=0;i<3;i++)
        {
            //if a = 0 b =0, noise = [-1,1]
            random[i] = noise_a * 2 * (((float)(rand() % 100))/100 - 0.5 )  + noise_b;
        }

        random[0] = LPF_x.apply(random[0], 0.01);
        random[1] = LPF_y.apply(random[1], 0.01);
        random[2] = LPF_z.apply(random[2], 0.01);


        for (int i=0;i<3;i++)
        {
            _DroneState.velocity[i] = _DroneState.velocity[i] + random[i];
        }

        //cout << "Random [X Y Z] : " << random[0]<<" " << random[1]<<" "<< random[2]<<endl;
        
                
        // 根据Use_mocap_raw来选择位置和速度的来源
        if (Use_mocap_raw == 1)
        {
            for (int i=0;i<3;i++)
            {
                _DroneState.position[i] = pos_drone_mocap[i];
            }
        }
        else if (Use_mocap_raw == 2) 
        {
            for (int i=0;i<3;i++)
            {
                _DroneState.position[i] = UAV_motion.position[i];
                _DroneState.velocity[i] = UAV_motion.velocity[i];
            }
        }
        //get payload state from mocap
        for (int i = 0;i<3;i++)
        {
           _DroneState.payload_vel[i] = Payload_motion.velocity[i];
           _DroneState.payload_pos[i] = Payload_motion.position[i];
        } 
        drone_state_pub.publish(_DroneState);

        rate.sleep();
    }

    return 0;

}


void publish_drone_state()
{

}

void send_to_fcu()
{
    geometry_msgs::PoseStamped vision;
    
    //vicon
    if(flag_use_laser_or_vicon == 0)
    {
        vision.pose.position.x = pos_drone_mocap[0] ;
        vision.pose.position.y = pos_drone_mocap[1] ;
        vision.pose.position.z = pos_drone_mocap[2] ;

        vision.pose.orientation.x = q_mocap.x();
        vision.pose.orientation.y = q_mocap.y();
        vision.pose.orientation.z = q_mocap.z();
        vision.pose.orientation.w = q_mocap.w();

    }//laser
    else if (flag_use_laser_or_vicon == 1)
    {
        vision.pose.position.x = pos_drone_laser[0];
        vision.pose.position.y = pos_drone_laser[1];
        vision.pose.position.z = pos_drone_laser[2];

        vision.pose.orientation.x = q_laser.x();
        vision.pose.orientation.y = q_laser.y();
        vision.pose.orientation.z = q_laser.z();
        vision.pose.orientation.w = q_laser.w();
    }

    vision.header.stamp = ros::Time::now();
    vision_pub.publish(vision);
}

void printf_info()
{
    cout <<">>>>>>>>>>>>>>>>>>>>>>>>PX4_POS_ESTIMATOR<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<" <<endl;

    //固定的浮点显示
    cout.setf(ios::fixed);
    //setprecision(n) 设显示小数精度为n位
    cout<<setprecision(2);
    //左对齐
    cout.setf(ios::left);
    // 强制显示小数点
    cout.setf(ios::showpoint);
    // 强制显示符号
    cout.setf(ios::showpos);

    //using vicon system
    if(flag_use_laser_or_vicon == 0)
    {
        cout <<">>>>>>>>>>>>>>>>>>>>>>>>Vicon Info [ENU Frame]<<<<<<<<<<<<<<<<<<<<<<<<<" <<endl;
        cout << "Pos_vicon [X Y Z] : " << pos_drone_mocap[0] << " [ m ] "<< pos_drone_mocap[1] <<" [ m ] "<< pos_drone_mocap[2] <<" [ m ] "<<endl;
        cout << "Att_vicon [R P Y] : " << Euler_mocap[0] * 180/M_PI <<" [deg] "<< Euler_mocap[1] * 180/M_PI << " [deg] "<< Euler_mocap[2] * 180/M_PI<<" [deg] "<<endl;
    }else
    {
        cout <<">>>>>>>>>>>>>>>>>>>>>>>>Laser Info [ENU Frame]<<<<<<<<<<<<<<<<<<<<<<<<<" <<endl;
        cout << "Pos_laser [X Y Z] : " << pos_drone_laser[0] << " [ m ] "<< pos_drone_laser[1] <<" [ m ] "<< pos_drone_laser[2] <<" [ m ] "<<endl;
        cout << "Euler_laser[Yaw] : " << Euler_laser[2] * 180/M_PI<<" [deg]  "<<endl;
    }

        cout <<">>>>>>>>>>>>>>>>>>>>>>>>FCU Info [ENU Frame]<<<<<<<<<<<<<<<<<<<<<<<<<<<" <<endl;
        cout << "Pos_fcu [X Y Z] : " << pos_drone_fcu[0] << " [ m ] "<< pos_drone_fcu[1] <<" [ m ] "<< pos_drone_fcu[2] <<" [ m ] "<<endl;
        cout << "Vel_fcu [X Y Z] : " << vel_drone_fcu[0] << " [m/s] "<< vel_drone_fcu[1] <<" [m/s] "<< vel_drone_fcu[2] <<" [m/s] "<<endl;
        cout << "Att_fcu [R P Y] : " << Att_fcu[0] * 180/M_PI <<" [deg] "<< Att_fcu[1] * 180/M_PI << " [deg] "<< Att_fcu[2] * 180/M_PI<<" [deg] "<<endl;
        cout << "Payload Inertial Velocity [v]:" << _DroneState.payload_vel[0]<< "[m/s]"<<_DroneState.payload_vel[1]<< "[m/s]"<<_DroneState.payload_vel[2]<< "[m/s]"<<endl;
}

void printf_param()
{
    cout <<">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> Parameter <<<<<<<<<<<<<<<<<<<<<<<<<<<" <<endl;

    cout << "noise_a: "<< noise_a<<" [m] "<<endl;
    cout << "noise_b: "<< noise_b<<" [m] "<<endl;
    cout << "noise_T: "<< noise_T<<" [m] "<<endl;
    

}