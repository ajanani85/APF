#include <ros/ros.h>
#include <tf/tf.h>
#include <sensor_msgs/LaserScan.h>
#include <ploughing_reversible/ReachPoint.h>
#include <nav_msgs/Odometry.h>
#include <geometry_msgs/Twist.h>
#include <geometry_msgs/Quaternion.h>
#include <string>
#include <pthread.h>
#include <math.h>
#include <sstream>
#include <stdlib.h>     /* srand, rand */
#include <time.h> 


#define PI 3.14159265359
#define Rad2Deg 57.2957795
#define Deg2Rad 0.0174532925
#define PITCH 0.3522504892367
#define DistanceTh 0.2
#define DistanceCrt 1.5
#define DistanceCrtTwo 0.5
#define MaxVel 0.5
#define AlgorithmEffectDistance 5.5

using namespace std;

struct force
{
	float mag;
	float theta;
	float x;
	float y;
};

struct pose
{
	float x;
	float y;
	double theta;
};
struct mapMem
{
	float Rx;
	float Ry;
	float Ox;
	float Oy;
	int time;
	float lr;
	float lr_ang;
	float lrx;
	float lry;
};
mapMem fmm_old;
int fjcount = 0;
float fjmean= 0.0;
pose current, ab[10];

void stop(void);
void *move(void* threadID);
geometry_msgs::Twist vel;
ploughing_reversible::ReachPoint target,reqPoint, prevPoint;
pose currentGlobal, requestedTarget;
bool targetIsReached = false, headingIsCorrected = false;
pthread_t moveThHandler_;
string procPointHint="";
int robot_id = 0;
float dl = 0.5, df = 3.0, dr = 1.5;
float reversCoef = 1.0;
//global Publishers which allow to access them from any thread and functions they will be assigned within ReachPoint class.
ros::Publisher vel_pub;
ros::Publisher target_status_pub;


class ReachPoint
{
	public:
		ReachPoint();
	private:
		ros::NodeHandle nh;
		ros::Subscriber target_sub;
		ros::Subscriber laser_sub;
		ros::Subscriber world_pose_sub;

		void laserCallBack(const sensor_msgs::LaserScan::ConstPtr &scan);
		void worldPoseCallback(const nav_msgs::Odometry::ConstPtr &world_pose);
		void targetCallback(const ploughing_reversible::ReachPoint::ConstPtr &target);
};

ReachPoint::ReachPoint()
{
	ros::NodeHandle n("~");
	n.getParam("robot_id",robot_id);
	ostringstream ss;
	ss << "robot_";
	ss << robot_id;
	string robot_name = ss.str();
	string laser_pipename = robot_name + "/base_scan";
	string pose_pipename = robot_name + "/base_pose_ground_truth";
	string twist_pipename = robot_name + "/cmd_vel";
	string pointreq_pipename = robot_name + "/requested_point";
	string pointres_pointname = robot_name + "/reach_point_status";
	target_sub = nh.subscribe<ploughing_reversible::ReachPoint>(pointreq_pipename,5,&ReachPoint::targetCallback,this);
	laser_sub = nh.subscribe<sensor_msgs::LaserScan>(laser_pipename,5,&ReachPoint::laserCallBack,this);
	world_pose_sub = nh.subscribe<nav_msgs::Odometry>(pose_pipename,5,&ReachPoint::worldPoseCallback,this);
	vel_pub = nh.advertise<geometry_msgs::Twist>(twist_pipename,1);
	target_status_pub = nh.advertise<ploughing_reversible::ReachPoint>(pointres_pointname,5);

	for (int i=0; i<=9; i++)
	{
		ab[i].x = 10 - AlgorithmEffectDistance - i*1;
		ab[i].y = -13;
	}


}
//a method to receive laser range finder readings.
void ReachPoint::laserCallBack(const sensor_msgs::LaserScan::ConstPtr &scan)
{
	bool roadIsBlocked = false, rightIsBlocked = false, leftIsBlocked = false;
	float frontObstacleMax = 4.5, FOBMaxAngle = 0.0,leftObstacleMax = 4.5, LOBMaxAngle = 0.0, rightObstacleMax = 4.5, ROBMaxAngle = 0.0;
	pose dir;
	
	if (reqPoint.hint.compare("done") == 0)
	{	
		//For APF
		float beams[512];
		pose obj_new[512], obj[512];
		force attraction, repulsion[512], sum;
		int RotCoef = 1;
		sum.x = 0.0;
		sum.y = 0.0;
		int beamCounter = 1;
		dir.x = requestedTarget.x - currentGlobal.x;
		dir.y = requestedTarget.y - currentGlobal.y;
		dir.theta = atan2(dir.y,dir.x)*Rad2Deg;
		float distanceToTarget = sqrt(dir.x*dir.x + dir.y*dir.y);
		float R[2][2] = {{cos((90-currentGlobal.theta)*Deg2Rad), -1*sin((90-currentGlobal.theta)*Deg2Rad)},
											 {sin((90-currentGlobal.theta)*Deg2Rad), cos((90-currentGlobal.theta)*Deg2Rad)}};
		if (distanceToTarget >= 3.0)
		{		
			for (int i=0; i<=511; i++)
			{
				if (scan->ranges[i] < 4.5 )
				{
					beams[i] = scan->ranges[i];
				
					obj[i].x = beams[i]*cos((i*PITCH)*Deg2Rad);
					obj[i].y = beams[i]*sin((i*PITCH)*Deg2Rad);
					beamCounter++;
				}
				else if (scan->ranges[i] == 4.5)
				{
					beams[i] = 0.0;
					obj[i].x = 0.0;
					obj[i].y = 0.0;
				}
			}
		
			//form the orientation matrix
			for (int i=0; i<= 511; i++)
			{
				if (beams[i] > 0.0 && beams[i] < 4.5)
				{
				
					repulsion[i].x = obj[i].x;
					repulsion[i].y = obj[i].y;
				
					//remember: repulsion force is 180 degrees mirrored of the vector describing the object and the robot
					if (beams[i] <= DistanceCrt && beams[i] > DistanceCrtTwo)
					{
						repulsion[i].x = 20*obj[i].x;
						repulsion[i].y = 20*obj[i].y;
						RotCoef = 10;
					}
					if (beams[i] <= DistanceCrtTwo)
					{
						repulsion[i].x = 100*obj[i].x;
						repulsion[i].y = 100*obj[i].y;
						RotCoef = 100;
					}
					if (beams[i] > DistanceCrt)
					{
						repulsion[i].x = obj[i].x;
						repulsion[i].y = obj[i].y;
						RotCoef = 1;
					}
					repulsion[i].mag = 1/sqrt(repulsion[i].x*repulsion[i].x+repulsion[i].y*repulsion[i].y);
					repulsion[i].theta = atan2(repulsion[i].y,repulsion[i].x);
				}
				else if (beams[i] == 0.0)
				{
					repulsion[i].x = 0.0;
					repulsion[i].y = 0.0;
					repulsion[i].mag = 0.0;
					repulsion[i].theta = 0.0;
				}
			
				sum.x = sum.x + repulsion[i].x;
				sum.y = sum.y + repulsion[i].y;
			}
			//ROS_INFO("repulsion components 1: x:%f, y:%f, mag: %f, theta: %f",sum.x,sum.y, sum.mag, sum.theta);	
		
		
			float rx = sum.x*cos(180*Deg2Rad) - sum.y*sin(180*Deg2Rad);
			float ry = sum.x*sin(180*Deg2Rad) + sum.y*cos(180*Deg2Rad);		
			sum.x	= rx*R[0][0] - ry*R[0][1];
			sum.y = -rx*R[1][0] + ry*R[1][1];
			sum.mag = sqrt(sum.x*sum.x+sum.y*sum.y);
			sum.theta = Rad2Deg*atan2(sum.y,sum.x);
		
			//applying robot's orientation
		
			//ROS_INFO("repulsion components 2: x:%f, y:%f, mag: %f, theta: %f",sum.x,sum.y, sum.mag, sum.theta);	
			//attraction force
			attraction.mag = sqrt(dir.x*dir.x+dir.y*dir.y);
			attraction.theta = Rad2Deg*atan2(dir.y,dir.x);
			attraction.x = dir.x;
			attraction.y = dir.y;
			//ROS_INFO("attraction components: x:%f, y:%f, mag: %f, theta: %f",attraction.x,attraction.y, attraction.mag, attraction.theta);
			//sum force	
			int Krep = 7;
			int Katt = 20;
			sum.x = sum.x + Katt*attraction.x;
			sum.y = sum.y + Katt*attraction.y;
			sum.theta = Rad2Deg*atan2(sum.y,sum.x);
			sum.mag = sqrt(sum.x*sum.x+sum.y*sum.y);
			//ROS_INFO("sum force components: x:%f,y:%f -> mag:%f, angle:%f",sum.x,sum.y,sum.mag,sum.theta);
		
			/*Here is where you assign speed to the motors*/
			if (abs(sum.theta-currentGlobal.theta) < 180)
			{
				vel.angular.z = RotCoef*0.02*(sum.theta-currentGlobal.theta);
			}
			else if ((sum.theta-currentGlobal.theta > 180) || (sum.theta-currentGlobal.theta < -180))
			{
				vel.angular.z = RotCoef*-0.02*(sum.theta-currentGlobal.theta);
			}
			ROS_INFO("Sum components 2: x:%f, y:%f, mag: %f, theta: %f",sum.x,sum.y, sum.mag, sum.theta);
			vel.linear.x = sum.mag;
			//limiting the linear speed to 0.75 m/s
			if (vel.linear.x >= MaxVel)
			{
				vel.linear.x = MaxVel;
			}
			if (vel.linear.x <= -1*MaxVel)
			{
				vel.linear.x = -1*MaxVel;
			}
			vel_pub.publish(vel);

		}
		else if (distanceToTarget < 3.0)
		{
			stop();
			reqPoint.status = true;
			prevPoint.hint = reqPoint.hint;
			target_status_pub.publish(reqPoint);
			procPointHint = reqPoint.hint;
		}
	}	
	/*check the "reqPoint.hint" to assign limitation on field of view*/
	//if (reqPoint.hint.compare("ab1") == 0 || reqPoint.hint.compare("ab1-adjust") == 0 || reqPoint.hint.compare("ab1-left") == 0) 
	//{
	else if (reqPoint.hint.compare("done") != 0)
	{
			dir.x = requestedTarget.x - currentGlobal.x;
			dir.y = requestedTarget.y - currentGlobal.y;
			//|| (reqPoint.hint.compare("ab-adjust") == 0 && sqrt(dir.x*dir.x + dir.y*dir.y) >= AlgorithmEffectDistance/2) || (reqPoint.hint.compare("ab1-left") && sqrt(dir.x*dir.x + dir.y*dir.y) >= AlgorithmEffectDistance/2)
			if ((reqPoint.hint.compare("ab1") == 0 && sqrt(dir.x*dir.x + dir.y*dir.y) >= AlgorithmEffectDistance) || 
					(reqPoint.hint.compare("ab1-left") == 0 && sqrt(dir.x*dir.x + dir.y*dir.y) >= AlgorithmEffectDistance - 1.5) ||
					(reqPoint.hint.compare("ab1-adjust") == 0 && sqrt(dir.x*dir.x + dir.y*dir.y) >= AlgorithmEffectDistance - 1.5) ||
					(reqPoint.hint.compare("ab-resume") == 0 && sqrt(dir.x*dir.x + dir.y*dir.y) >= AlgorithmEffectDistance - 1.0))
			{
				dir.x = requestedTarget.x - currentGlobal.x;
				dir.y = requestedTarget.y - currentGlobal.y;
				dir.theta = atan2(dir.y,dir.x)*Rad2Deg;
				float distanceToTarget = sqrt(dir.x*dir.x + dir.y*dir.y);
				//For APF
				float beams[512];
				pose dir, obj_new[512], obj[512];
				force attraction, repulsion[512], sum;
				int RotCoef = 1;
				sum.x = 0.0;
				sum.y = 0.0;
				int beamCounter = 1;
				dir.x = requestedTarget.x - currentGlobal.x;
				dir.y = requestedTarget.y - currentGlobal.y;
				dir.theta = atan2(dir.y,dir.x)*Rad2Deg;
				distanceToTarget = sqrt(dir.x*dir.x + dir.y*dir.y);
				float R[2][2] = {{cos((90-currentGlobal.theta)*Deg2Rad), -1*sin((90-currentGlobal.theta)*Deg2Rad)},
													 {sin((90-currentGlobal.theta)*Deg2Rad), cos((90-currentGlobal.theta)*Deg2Rad)}};
				if (distanceToTarget >= DistanceTh)
				{		
					for (int i=0; i<=511; i++)
					{
						if (scan->ranges[i] < 4.5 )
						{
							beams[i] = scan->ranges[i];
				
							obj[i].x = beams[i]*cos((i*PITCH)*Deg2Rad);
							obj[i].y = beams[i]*sin((i*PITCH)*Deg2Rad);
							beamCounter++;
						}
						else if (scan->ranges[i] == 4.5)
						{
							beams[i] = 0.0;
							obj[i].x = 0.0;
							obj[i].y = 0.0;
						}
					}
		
					//form the orientation matrix
					for (int i=0; i<= 511; i++)
					{
						if (beams[i] > 0.0 && beams[i] < 4.5)
						{
				
							repulsion[i].x = obj[i].x;
							repulsion[i].y = obj[i].y;
				
							//remember: repulsion force is 180 degrees mirrored of the vector describing the object and the robot
							if (beams[i] <= DistanceCrt && beams[i] > DistanceCrtTwo)
							{
								repulsion[i].x = 20*obj[i].x;
								repulsion[i].y = 20*obj[i].y;
								RotCoef = 10;
							}
							if (beams[i] <= DistanceCrtTwo)
							{
								repulsion[i].x = 100*obj[i].x;
								repulsion[i].y = 100*obj[i].y;
								RotCoef = 100;
							}
							if (beams[i] > DistanceCrt)
							{
								repulsion[i].x = obj[i].x;
								repulsion[i].y = obj[i].y;
								RotCoef = 1;
							}
							repulsion[i].mag = 1/sqrt(repulsion[i].x*repulsion[i].x+repulsion[i].y*repulsion[i].y);
							repulsion[i].theta = atan2(repulsion[i].y,repulsion[i].x);
						}
						else if (beams[i] == 0.0)
						{
							repulsion[i].x = 0.0;
							repulsion[i].y = 0.0;
							repulsion[i].mag = 0.0;
							repulsion[i].theta = 0.0;
						}
			
						sum.x = sum.x + repulsion[i].x;
						sum.y = sum.y + repulsion[i].y;
						
					}
					//ROS_INFO("repulsion components 1: x:%f, y:%f, mag: %f, theta: %f",sum.x,sum.y, sum.mag, sum.theta);	
		
		
					float rx = sum.x*cos(180*Deg2Rad) - sum.y*sin(180*Deg2Rad);
					float ry = sum.x*sin(180*Deg2Rad) + sum.y*cos(180*Deg2Rad);		
					sum.x	= rx*R[0][0] - ry*R[0][1];
					sum.y = -rx*R[1][0] + ry*R[1][1];
					
		
					////////////////////////////////Preventing the robot to enter the field///////////////////////////////////////////////////					
					if (currentGlobal.y < -15.0 && (-15.0 - currentGlobal.y <= 2.0) && currentGlobal.x <= 11.0 && currentGlobal.x >= -11.0)
					{
						sum.x += 10000.00;
						sum.y += -10000.00;	
					}
					else if (currentGlobal.y < 13.0 && currentGlobal.y > -13 && currentGlobal.x >= 12.0 && (currentGlobal.x - 12 <= 0.75))
					{
						sum.x += 10000.00;						
						sum.y += 10000.00;	
					}
					else if (currentGlobal.y > 12.0 && (currentGlobal.y - 13.0 <= 2.0) && currentGlobal.x <= 11.0 && currentGlobal.x > 0.0)
					{
						sum.x += -10000.0;
						sum.y += 10000.00;
					}
					else if(currentGlobal.y > 12.0 && (currentGlobal.y - 13 <= 2.0) && currentGlobal.x <= 0.0 && currentGlobal.x >= -11.0)
					{
						sum.x += 10000.00;
						sum.y += 10000.00;
					}
					else if (currentGlobal.y < 13.0 && currentGlobal.y > -13.0 && currentGlobal.x <= -12.0 && ( -12 - currentGlobal.x <= 0.75))
					{
						sum.x += -10000.00;						
						sum.y += 10000.00;	
					}
					
					sum.mag = sqrt(sum.x*sum.x+sum.y*sum.y);
					sum.theta = Rad2Deg*atan2(sum.y,sum.x);
					//ROS_INFO("repulsion components 2: x:%f, y:%f, mag: %f, theta: %f",sum.x,sum.y, sum.mag, sum.theta);	
					//attraction force
					attraction.mag = sqrt(dir.x*dir.x+dir.y*dir.y);
					attraction.theta = Rad2Deg*atan2(dir.y,dir.x);
					attraction.x = dir.x;
					attraction.y = dir.y;
					//ROS_INFO("attraction components: x:%f, y:%f, mag: %f, theta: %f",attraction.x,attraction.y, attraction.mag, attraction.theta);
					//sum force	
					int Krep = 7;
					int Katt = 20;
					sum.x = sum.x + Katt*attraction.x;
					sum.y = sum.y + Katt*attraction.y;
					sum.theta = Rad2Deg*atan2(sum.y,sum.x);
					sum.mag = sqrt(sum.x*sum.x+sum.y*sum.y);
					//ROS_INFO("sum force components: x:%f,y:%f -> mag:%f, angle:%f",sum.x,sum.y,sum.mag,sum.theta);
		
					/*Here is where you assign speed to the motors*/
					if (abs(sum.theta-currentGlobal.theta) < 180)
					{
						vel.angular.z = RotCoef*0.02*(sum.theta-currentGlobal.theta);
					}
					else if ((sum.theta-currentGlobal.theta > 180) || (sum.theta-currentGlobal.theta < -180))
					{
						vel.angular.z = RotCoef*-0.02*(sum.theta-currentGlobal.theta);
					}
					ROS_INFO("Sum components 2: x:%f, y:%f, mag: %f, theta: %f",sum.x,sum.y, sum.mag, sum.theta);
					vel.linear.x = sum.mag;
					//limiting the linear speed to 0.75 m/s
					if (vel.linear.x >= MaxVel)
					{
						vel.linear.x = MaxVel;
					}
					if (vel.linear.x <= -1*MaxVel)
					{
						vel.linear.x = -1*MaxVel;
					}
					vel_pub.publish(vel);

				}
				else if (distanceToTarget <DistanceTh)
				{
					stop();
					reqPoint.status = true;
					prevPoint.hint = reqPoint.hint;
					target_status_pub.publish(reqPoint);
					procPointHint = reqPoint.hint;
				}		
			}
			else
			{
					for(int i=191; i<=319; i++)
					{	
						if(scan->ranges[i] <= df)
						{
							roadIsBlocked = true;
							if (scan->ranges[i] < frontObstacleMax)
							{
								frontObstacleMax = scan->ranges[i];
								FOBMaxAngle = i*PITCH*PI/180;
							}
						}
					}
					for(int j=0; j<=127; j++)
					{	
						if(scan->ranges[j] <= dr)
						{
							rightIsBlocked = true;
							if (scan->ranges[j] < rightObstacleMax)
							{
								rightObstacleMax = scan->ranges[j];
								ROBMaxAngle = j*PITCH*PI/180;
							}
						}
					}
					for(int k=385; k<=512; k++)
					{	
						if(scan->ranges[k] <= dl)
						{
							leftIsBlocked = true;
							if (scan->ranges[k] < leftObstacleMax)
							{
								leftObstacleMax = scan->ranges[k];
								LOBMaxAngle = k*PITCH*PI/180;
							}
						}
					}
				//}
				if (reqPoint.hint.compare("ab1") != 0 && reqPoint.hint.compare("ab1-adjust") != 0 
						&& reqPoint.hint.compare("ab1-left") != 0 && reqPoint.hint.compare("ab-resume") != 0)
				{
					rightIsBlocked = false;
					leftIsBlocked = false;
				}
	
			 /*this is a section which guides the robot, it has been put in here because 
			 	 this function is being updated 5 times a second. Remeber because is being 
				 called very often, we are not allowed to put any while loop in here, if we
				 do that, laser range finder readings will be missed. 
			 */
				pose dir, prevdir;
				dir.x = requestedTarget.x - currentGlobal.x;
				dir.y = requestedTarget.y - currentGlobal.y;
				dir.theta = atan2(dir.y,dir.x)*Rad2Deg;
				prevdir = dir;
				/*if (dir.theta <= 180.0 && dir.theta >= 178.0)
				{
					dir.theta = 179.0;
				}
				if (dir.theta >= -180.0 && dir.theta <= -178.0)
				{
					dir.theta = -179.0;
				}*/	

				if(abs(dir.theta-currentGlobal.theta) >= 10.0 && (reqPoint.hint.compare("cd") == 0 || reqPoint.hint.compare("ab") == 0) && prevPoint.hint.compare(reqPoint.hint.c_str()) != 0)
				{
					//heading correction
					if (abs(dir.theta-currentGlobal.theta) < 180)
					{
						vel.angular.z = 0.04*(dir.theta-currentGlobal.theta);
					}
					else if ((dir.theta-currentGlobal.theta > 180) || (dir.theta-currentGlobal.theta < -180))
					{
						vel.angular.z = -0.04*(dir.theta-currentGlobal.theta);
					}
					vel.linear.x = 0.0;
					vel_pub.publish(vel);
					headingIsCorrected = false;
				}
				else if (reqPoint.hint.compare("cd") != 0 || reqPoint.hint.compare("ab") != 0)
				{
					headingIsCorrected = true;
				}
				//else if(abs(dir.theta-currentGlobal.theta) < 2.0 && (reqPoint.hint.compare("cd") == 0 || reqPoint.hint.compare("ab") == 0))
				//{
					if(!targetIsReached && headingIsCorrected)
					{
						//Congestion avoidance
						if ((roadIsBlocked || rightIsBlocked) && sqrt(dir.x*dir.x + dir.y*dir.y) <= frontObstacleMax*sin(FOBMaxAngle)/2)
						{
							//ROS_INFO("robot_%d detected a congestion", robot_id);
							roadIsBlocked = false;
							rightIsBlocked = false;
						}
						//if lasers didn't detect anything in the field, approach the target.
						if(!roadIsBlocked && !rightIsBlocked && !leftIsBlocked)
						{
							//update the position of the robot respect to target
							dir.x = requestedTarget.x - currentGlobal.x;
							dir.y = requestedTarget.y - currentGlobal.y;
							dir.theta = atan2(dir.y,dir.x)*Rad2Deg;
							//simulation cannot reach 180 or -180 degrees.
				
							if (abs(dir.theta-currentGlobal.theta) < 180)
							{
								vel.angular.z = 0.02*(dir.theta-currentGlobal.theta);
							}
							else if ((dir.theta-currentGlobal.theta > 180) || (dir.theta-currentGlobal.theta < -180))
							{
								vel.angular.z = -0.02*(dir.theta-currentGlobal.theta);
							}
					 		vel.linear.x = sqrt(dir.x*dir.x + dir.y*dir.y);
							//limiting the linear speed to 0.75 m/s
							if (vel.linear.x >= 0.75)
							{
								vel.linear.x = 0.75;
							}
							if (vel.linear.x <= -0.75)
							{
								vel.linear.x = -0.75;
							}
							vel_pub.publish(vel);
							prevdir = dir;
						}
						//if any obstacle is detected within the field of view of the robot
					 else if(roadIsBlocked || rightIsBlocked || leftIsBlocked)
					 {
							if (reqPoint.hint.compare("ab1") == 0 || reqPoint.hint.compare("ab1-adjust") == 0 || reqPoint.hint.compare("ab1-left") == 0 || reqPoint.hint.compare("ab-resume") == 0)
							{
								//if (prevPoint.hint.compare("cd") != 0)
								//{
										reversCoef = rand() % 20 + 1;
										vel.linear.x = -1*reversCoef*0.02;
										vel.angular.z = 0.0;
										vel_pub.publish(vel);
										//ROS_INFO("robot_%d has V = %f",robot_id, vel.linear.x);
										prevdir = dir;
								//}
								/*else if (prevPoint.hint.compare("cd") == 0)
								{
									vel.linear.x = 0.0;
									vel.angular.z = 0.0;
									vel_pub.publish(vel);
								}*/
								
							}
							else if (reqPoint.hint.compare("cd") == 0 || reqPoint.hint.compare("ab") == 0)
							{
								vel.linear.x = 0.0;
								vel.angular.z = 0.0;
								vel_pub.publish(vel);
							}
					 }
					}
				//}
				//if we have reached in distance of 5 cm or less of the target, target is assumed to be reached.
				if (sqrt(dir.x*dir.x + dir.y*dir.y) <= 0.20)
				{
						targetIsReached = true;
						stop();
						reqPoint.status = true;
						prevPoint.hint = reqPoint.hint;
						target_status_pub.publish(reqPoint);
						procPointHint = reqPoint.hint;
				}
			}
		}
}

//a method to receive global position of the robot.
void ReachPoint::worldPoseCallback(const nav_msgs::Odometry::ConstPtr &world_pose)
{
	currentGlobal.x = world_pose->pose.pose.position.x;
	currentGlobal.y = world_pose->pose.pose.position.y;
	currentGlobal.theta = Rad2Deg * tf::getYaw(world_pose->pose.pose.orientation);
	//ROS_INFO("Current Global Angle = %f",currentGlobal.theta);
}
//a method to receive target coordinates.
void ReachPoint::targetCallback(const ploughing_reversible::ReachPoint::ConstPtr &target)
{
	//keeping record of the requested target
	reqPoint.point.x = target->point.x;
	reqPoint.point.y = target->point.y;
	reqPoint.hint = target->hint;
	reqPoint.status = target->status;
	//loading the target
	requestedTarget.x = target->point.x;
	requestedTarget.y = target->point.y;
	
	//start the move thread
	
  /*if (target->hint.compare(procPointHint) == 0 && procPointHint.compare("ab") != 0)
	{
		targetIsReached = true;
		headingIsCorrected = true;
		reqPoint.status = true;
		target_status_pub.publish(reqPoint);
	}
  else
	{*/
	if(reqPoint.hint.compare("ab1") == 0)
	{
		df = 1.5;
		dl = 0.5;
		dr = 1.5;
	}
	else if(reqPoint.hint.compare("ab1-adjust") == 0)
	{
		df = 3.0;
		dl = 1.0;
		dr = 3.0;
	}
	else if(reqPoint.hint.compare("ab") == 0)
	{
		df = 0.5;
		dl = 0.0;
		dr = 0.0;
	}
	else if(reqPoint.hint.compare("cd") == 0 )
	{
		df = 1.0;
		dl = 0.0;
		dr = 0.0;
	}
	else if(reqPoint.hint.compare("ab1-left") == 0 || reqPoint.hint.compare("done") == 0)
	{
		df = 1.0;
		dl = 0.5;
		dr = 0.75;
	}
	targetIsReached = false;
	headingIsCorrected = false;
	//}
}

void stop(void)
{
	vel.linear.x = 0.0;
	vel.linear.y = 0.0;
	vel.linear.z = 0.0;
	vel.angular.x = 0.0;
	vel.angular.y = 0.0;
	vel.angular.z = 0.0;
	vel_pub.publish(vel);
}
int main(int argc, char** argv)
{
	ros::init(argc,argv,"reach_point");
	ReachPoint rp;
	srand(time(NULL));
	ros::spin();
	return 0;
}
