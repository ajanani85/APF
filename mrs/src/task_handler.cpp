#include <ros/ros.h>
#include <iostream>
#include <sstream>
#include <nav_msgs/Odometry.h>
#include <ploughing_reversible/ReachPoint.h>
#include <ploughing_reversible/occupied.h>
#include <stdio.h>      /* printf, scanf, puts, NULL */
#include <stdlib.h>     /* srand, rand */
#include <time.h>       /* time */

using namespace std;

int robot_id = 0;
string robot_name;
float cpX, cpY;
bool sysIsReady = false;
int p_index = 0;
struct pose
{
	float x;
	float y;
};

//Method Signitures
list<pose> furrowLocalizer(pose start, pose end, float dist);
pose getComponent(list<pose> _list, int _i);
int getIndex(list<pose> _list, pose _pose);
void map_init(void);
//Variables
pose alpha,A,B,C,D, lastKnownPose, newPose, topAd, bottomAd, leftAd;
list<pose> furCoordAB, furCoordCD, homeCD, homeAB;
float furDist, furTransAB, furTransCD;
int furTotal;
ploughing_reversible::ReachPoint reach;
ploughing_reversible::occupied checkPoint,updatePoint;

ros::Publisher reach_point_pub;


class THClass
{
	public:
		THClass();
	private:
		//a normal node handle for general tasks
		ros::NodeHandle n;
		ros::Subscriber pose_sub;
		ros::Subscriber reach_point_status_sub;
		
		ros::ServiceClient ask_reference;
		ros::ServiceClient update_reference;


		void reachPointStatusCallback(const ploughing_reversible::ReachPoint::ConstPtr &reached_point);
		void poseCB(const nav_msgs::Odometry::ConstPtr &pose);
};

THClass::THClass()
{
	//a private node handle for collecting parameters
	ros::NodeHandle nh("~");
	ostringstream ss;
	nh.getParam("robot_id",robot_id);
	ss << "robot_";
	ss << robot_id;
	robot_name = ss.str();
	string pose_pipename = robot_name+"/base_pose_ground_truth";
	string pointreq_pipename = robot_name + "/requested_point";
	string pointres_pointname = robot_name + "/reach_point_status";
	pose_sub = n.subscribe<nav_msgs::Odometry>(pose_pipename,10,&THClass::poseCB,this);	
	reach_point_status_sub = n.subscribe<ploughing_reversible::ReachPoint>(pointres_pointname,100,&THClass::reachPointStatusCallback,this);
	reach_point_pub = n.advertise<ploughing_reversible::ReachPoint>(pointreq_pipename,10);
	ask_reference = n.serviceClient<ploughing_reversible::occupied>("check_coordinate");
	update_reference = n.serviceClient<ploughing_reversible::occupied>("add_coordinate");
	
}
void THClass::poseCB(const nav_msgs::Odometry::ConstPtr &pose)
{
	cpX = pose->pose.pose.position.x;
	cpY = pose->pose.pose.position.y;
	
	if (sysIsReady && reach_point_pub.getNumSubscribers() != 0)
	{
		if (cpX <= C.x && cpY >= C.y)
		{
			reach.point.x = topAd.x;
			reach.point.y = topAd.y;
			ROS_INFO("%s: Going to ab1-bottom-adjust", robot_name.c_str());
			reach.hint = "ab1-adjust";
			reach.status = false;
			reach_point_pub.publish(reach);
		}
		else if (cpX <= B.x && cpY >= B.y && cpY <= D.y)
		{
			reach.point.x = bottomAd.x;
			reach.point.y = bottomAd.y;
			ROS_INFO("%s: Going to ab1-top-adjust", robot_name.c_str());
			reach.hint = "ab1-adjust";
			reach.status = false;
			reach_point_pub.publish(reach);
		}
		else
		{
			newPose = getComponent(furCoordAB,0);
			reach.point.x = newPose.x;
			reach.point.y = newPose.y;
			ROS_INFO("%s: Going to ab1", robot_name.c_str());
			reach.hint = "ab1";
			reach.status = false;
			reach_point_pub.publish(reach);
		}
		sysIsReady = false;
	}
}

void THClass::reachPointStatusCallback(const ploughing_reversible::ReachPoint::ConstPtr &reached_point)
{
	string target_name = reached_point->hint;
	pose reached_location;
	reached_location.x = reached_point->point.x;
	reached_location.y = reached_point->point.y;
	if (target_name.compare("ab1-left") == 0)
	{
		reach.point.x = bottomAd.x;
		reach.point.y = bottomAd.y;
		//ROS_INFO("%s: Going to ab1-top-adjust", robot_name.c_str());
		reach.hint = "ab1-adjust";
		reach.status = false;
		reach_point_pub.publish(reach);
	}
	if (target_name.compare("ab1-adjust") == 0)
	{
		if (p_index < int(furTotal/2))
		{
			pose newPose = getComponent(furCoordAB,0);
			reach.point.x = newPose.x;
			reach.point.y = newPose.y;
			reach.hint = "ab1";
			reach.status = false;
			reach_point_pub.publish(reach);
		}
		else if (p_index >= int(furTotal/2))
		{
			pose newPose = getComponent(furCoordAB,p_index);
			reach.point.x = newPose.x;
			reach.point.y = newPose.y;
			reach.hint = "ab-resume";
			reach.status = false;
			reach_point_pub.publish(reach);
		}
	}
	if (target_name.compare("ab1") == 0 || target_name.compare("ab") == 0 || target_name.compare("ab-resume") == 0)
	{
		pose temp = getComponent(furCoordAB,p_index);
		checkPoint.request.x = temp.x;
		checkPoint.request.y = temp.y;
		if (p_index < furTotal - 1)
		{
			if(ask_reference.call(checkPoint))
			{
					//if the requested point is occupied
					if (checkPoint.response.status == true)
					{
						//go to the next ploughing target
						p_index = p_index + 1;
						newPose = getComponent(furCoordAB,p_index);
						reach.point.x = newPose.x;
						reach.point.y = newPose.y;
						reach.hint = "ab";
						reach.status = true;
						reach_point_pub.publish(reach);
					}
					//if the requested point is not occupied.
					else if (checkPoint.response.status == false)
					{
						//update reference
						pose temp = getComponent(furCoordAB,p_index);
						
						updatePoint.request.x = temp.x;
						updatePoint.request.y = temp.y;
						if(update_reference.call(updatePoint))
						{
								//start plouging
								newPose = getComponent(furCoordCD,p_index);
								reach.point.x = newPose.x;
								reach.point.y = newPose.y;
								reach.hint = "cd";
								reach.status = false;
								reach_point_pub.publish(reach);
						}
					}
			}
		}
		else if (p_index >= furTotal-1)
		{
			srand (time(NULL));
			float randx = -1*rand()%60+30; //a random number between 5 and 15
			usleep(500000);
			srand (time(NULL));
			float randy = -1*(rand()%30+15); //a random number between -1 and 1
			
			 
			reach.point.x = cpX + randx;
			reach.point.y = cpY + randy;
			reach.hint = "done";
			reach.status = false;
			reach_point_pub.publish(reach);
			ROS_INFO("robot_%d random Home location = %f %f",robot_id,reach.point.x, reach.point.y);
		}
	}
	if (target_name.compare("cd") == 0)
	{
		if (getIndex(furCoordCD,reached_location) < furTotal-1)
		{
			//decide which direction is closer
			if (getIndex(furCoordCD,reached_location) >= int(furTotal/2))
			{
				reach.point.x = leftAd.x;
				reach.point.y = leftAd.y;
				reach.hint = "ab1-left";
				reach.status = false;
				reach_point_pub.publish(reach);
			}
			else if (getIndex(furCoordCD,reached_location) < int(furTotal/2))
			{
				reach.point.x = topAd.x;
				reach.point.y = topAd.y;
				reach.hint = "ab1-adjust";
				reach.status = false;
				reach_point_pub.publish(reach);
			}
		}
		else 
		{
			reach.point.x = cpX;
			reach.point.y = cpY+3.0;
			reach.hint = "done";
			reach.status = false;
			reach_point_pub.publish(reach);
		}
	}
}
int main(int argc, char **argv)
{
	map_init();
	ros::init(argc,argv,"task_handler");
	THClass th;
	sleep(3);
	sysIsReady = true;
	
	ros::spin();
	return 0;
}
//a method to map the furrow coordinates. it collects all calculated coordinates in a list.
list<pose> furrowLocalizer(pose start, pose end, float dist)
{
	list<pose> createdList;
	pose currentPose = start;
	while(currentPose.x >= end.x)
	{
		/*
			push_back -> add element at the end, last element.
			push_front -> add element to the front, element 0.		
		*/
		createdList.push_back(currentPose);
		currentPose.x -= dist;
	}
	return createdList;
}
//a method to get particular component in a given list. 
pose getComponent(list<pose> _list, int _i){
    list<pose>::iterator it = _list.begin();
    for(int i=0; i<_i; i++){
        ++it;
    }		
    return *it;
}
int getIndex(list<pose> _list, pose _pose)
{
	int index = -1;
	pose temp;
	for(int i=0; i<_list.size(); i++)
	{
				temp = getComponent(_list,i);
				if (temp.x == _pose.x && temp.y == _pose.y)
				{
					index = i;
				}
	}
	return index;
}
void map_init(void)
{
	A.x = 10.0;
	A.y = -13.0;
	B.x = -10.0;
	B.y = -13.0;
	C.x = 10.0;
	C.y = 13.0;
	D.x = -10.0;
	D.y = 13.0;
	bottomAd.x = B.x - 5.0;
	bottomAd.y = B.y - 5.0;
	topAd.x = C.x + 5.0;
	topAd.y = C.y + 1.0;
	leftAd.x = D.x - 6.0;
	leftAd.y = D.y + 1.0;
  furDist = 0.4;
	furTransAB = 4.0;
	furTransCD = 4.0;
	//ROS_INFO("Information Received, calculating furrow coordinates...");
	// calculate furrow coordinates
	furCoordAB = furrowLocalizer(A,B,furDist);
	//ROS_INFO("AB coordinates calculated.");
	furCoordCD = furrowLocalizer(C,D,furDist);
	//ROS_INFO("CD coordinates calculated.");
	furTotal = furCoordCD.size();
	//calculating homing coordinates
	C.y = C.y + furTransCD;
	D.y = D.y + furTransCD;
	A.y = A.y - furTransAB;
	B.y = B.y - furTransAB;
	homeCD = furrowLocalizer(C,D,furDist);
	homeAB = furrowLocalizer(A,B,furDist);
	
}
