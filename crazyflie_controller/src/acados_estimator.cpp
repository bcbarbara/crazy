/*
* The MIT License (MIT)
*
* Copyright 2020 Barbara Barros Carlos, Tommaso Sartor
*
* This file is part of crazyflie_nmpc.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/


// standard
#include <iostream>
#include <sstream>
#include <fstream>
#include <ios>

// linear algebra
//
// matrices and vectors
#include <eigen3/Eigen/Dense>
#include <eigen3/Eigen/Geometry>

// ros
//
#include <ros/ros.h>
// srv
#include <std_srvs/Empty.h>
// msg
#include <std_msgs/String.h>
#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/Twist.h>
#include <std_msgs/String.h>
#include <sensor_msgs/Imu.h>
#include <geometry_msgs/Quaternion.h>
#include <geometry_msgs/QuaternionStamped.h>
#include <geometry_msgs/Vector3Stamped.h>
#include <crazyflie_controller/GenericLogData.h>
#include <crazyflie_controller/EulerAnglesStamped.h>
#include <crazyflie_controller/CrazyflieStateStamped.h>
#include <crazyflie_controller/PropellerSpeedsStamped.h>

// dynamic reconfirgure
#include <dynamic_reconfigure/server.h>
#include <crazyflie_controller/crazyflie_estimatorConfig.h>
#include <boost/thread.hpp>
#include "boost/thread/mutex.hpp"

// acados
#include "acados/utils/print.h"
#include "acados_c/ocp_nlp_interface.h"
#include "acados_c/external_function_interface.h"
// blasfeo
#include "blasfeo/include/blasfeo_d_aux.h"
#include "blasfeo/include/blasfeo_d_aux_ext_dep.h"

// crazyflie_model
#include "crazyflie_model/crazyflie_model.h"
#include "acados_solver_crazyflie.h"
#include "acados_sim_solver_crazyflie.h"

// Sampling time
#define TS 0.015
// Number of states
#define NX 13
// Number of control inputs
#define NU 4
// Number of measurements/references on nodes 0..N-1
#define NY 17
// Number of predictions
#define NPU 2
// Number of measurements/references on node N
#define NYN 13

// Constants
#define pi 3.14159265358979323846
#define g0 9.80665

#define RECONFIGURE 0

external_function_param_casadi * forw_vde_casadi;
using namespace Eigen;
using std::ofstream;
using std::cout;
using std::endl;
using std::fixed;
using std::showpos;

// acados sim dims

class ESTIMATOR
	{
	enum systemStates{
		xq = 0,
		yq = 1,
		zq = 2,
		qw = 3,
		qx = 4,
		qy = 5,
		qz = 6,
		vbx = 7,
		vby = 8,
		vbz = 9,
		wx = 10,
		wy = 11,
		wz = 12
	};

	enum controlInputs{
		w1 = 0,
		w2 = 1,
		w3 = 2,
		w4 = 3
	};

	struct euler{
		double phi;
		double theta;
		double psi;
	};

	struct sim_solver_input{
		double x0[NX];
		double Td;
		double u0[NU];
		double u1[NU];
	};

	struct sim_solver_output{
		double status, cpu_time;
		double xn[NX];
	};

	// control
	ros::Publisher p_cf_state;
	ros::Publisher p_cf_euler;
	ros::Publisher p_cf_quaternion;
	// log
	// ros::Publisher p_cf_quat;
	// ros::Publisher p_cf_lvb;
	// ros::Publisher p_cf_avb;
	// ros::Publisher p_cf_lpg;
	//
	ros::Subscriber s_sensorfusion6;
	ros::Subscriber s_imu;
	ros::Subscriber s_eRaptor;
	ros::Subscriber s_euler;
	ros::Subscriber s_motors;
	// ros::Subscriber s_actual_motors;

	// parameters
	double frequency, delay;

	// Variables for joy callback
	double
		joy_roll,
		joy_pitch,
		joy_yaw,
		joy_thrust;

	unsigned int k,i,j,ii;

	// Variables for linear velocity LPF
	std::vector<double> x_samples;
	std::vector<double> y_samples;
	std::vector<double> z_samples;
	std::vector<double> vx_filter_samples;
	std::vector<double> vy_filter_samples;
	std::vector<double> vz_filter_samples;

	// Elapsed time
	double t0;

	// acados struct
	sim_solver_input sim_acados_in;
	sim_solver_output sim_acados_out;
	int sim_acados_status;

	// Variables for reading the IMU data
	float actual_wx;
	float actual_wy;
	float actual_wz;
	float actual_roll;
	float actual_pitch;
	float actual_yaw;

	// Variables for eRaptor data
	float actual_x;
	float actual_y;
	float actual_z;

	// Variables for sensor fusion 6
	float actual_qw;
	float actual_qx;
	float actual_qy;
	float actual_qz;

	int8_t MAX_ORDER = 20;
	std::vector<double> y;

	int32_t
		actual_m1,
		actual_m2,
		actual_m3,
		actual_m4,
		acados_w1_latest,
		acados_w2_latest,
		acados_w3_latest,
		acados_w4_latest,
		acados_w1_prelatest,
		acados_w2_prelatest,
		acados_w3_prelatest,
		acados_w4_prelatest;

	public:

	ESTIMATOR(ros::NodeHandle& n, double delay)
		{

		int status = 0;
		status = crazyflie_acados_sim_create();

		if (status){
			ROS_INFO_STREAM("acados_sim_create() returned status " << status << ". Exiting." << endl);
			exit(1);
		}

		// publish crazyflie state
		p_cf_state = n.advertise<crazyflie_controller::CrazyflieStateStamped>(
			"/cf_estimator/state_estimate",1);
		p_cf_euler = n.advertise<crazyflie_controller::EulerAnglesStamped>(
			"/cf_estimator/euler_angles", 5);

		// subscriber for the motion capture system position
		s_eRaptor = n.subscribe("/crazyflie/external_position", 5, &ESTIMATOR::eRaptorCallback, this);
		// subscriber for the IMU linear acceleration and angular velocities from acc and gyro
		s_imu = n.subscribe("/crazyflie/imu", 5, &ESTIMATOR::imuCallback, this);
		// subscriber for the IMU stabilizer euler angles
		s_euler = n.subscribe("/crazyflie/euler_angles", 5, &ESTIMATOR::eulerCallback, this);
		// subscriber for the motor speeds applied
		s_motors = n.subscribe("/crazyflie/acados_motvel",5, &ESTIMATOR::motorsCallback, this);
		// subscriber for sensor fusion returning the quaternion
		//s_sensorfusion6 = n.subscribe("/crazyflie/sf6",5, &ESTIMATOR::sensorfusion6Callback, this);

		// logs
		// subscriber for the motors rpm
		// s_actual_motors = n.subscribe("/crazyflie/log1", 5, &ESTIMATOR::actual_motorsCallback, this);


		// initialize sim_acados_in.x0
		for (int i=0; i++; i<NX) sim_acados_in.x0[i] = 0;
		sim_acados_in.x0[qw] = 1;

		// initialize control inputs
		for(unsigned int i=0; i < NU; i++) sim_acados_in.u0[i] = 0.0;
		for(unsigned int i=0; i < NU; i++) sim_acados_in.u1[i] = 0.0;

		// Set size of buffer  of the estimated linear velocities to zero
		x_samples.resize(5);
		y_samples.resize(5);
		z_samples.resize(5);

		// Set initial value of the buffer to zero
		for (unsigned int i = 0; i <= 4; i++) x_samples[i] = 0.0;
		for (unsigned int i = 0; i <= 4; i++) y_samples[i] = 0.0;
		for (unsigned int i = 0; i <= 4; i++) z_samples[i] = 0.0;

		// Set size of buffer
		vx_filter_samples.resize(5);
		vy_filter_samples.resize(5);
		vz_filter_samples.resize(5);

		for (unsigned int i = 0; i <= 4; i++) vx_filter_samples[i] = 0.0;
		for (unsigned int i = 0; i <= 4; i++) vy_filter_samples[i] = 0.0;
		for (unsigned int i = 0; i <= 4; i++) vz_filter_samples[i] = 0.0;

		y.resize(MAX_ORDER);
		for (unsigned int i = 0; i <= MAX_ORDER; i++) y[i] = 0.0;

		// Set elapsed time to zero initially
		t0 = 0.0;
		}

	void run(double frequency)
		{
		ros::NodeHandle node;
		ros::Timer timer = node.createTimer(ros::Duration(1.0/frequency), &ESTIMATOR::predictor, this);

		#if RECONFIGURE
			ROS_DEBUG("Setting up the dynamic reconfigure panel and server");
			dynamic_reconfigure::Server<crazyflie_controller::crazyflie_estimatorConfig> server;
			dynamic_reconfigure::Server<crazyflie_controller::crazyflie_estimatorConfig>::CallbackType f;
			f = boost::bind(&ESTIMATOR::callback_dynamic_reconfigure, this, _1, _2);
			server.setCallback(f);
		#endif

		ros::spin();
		}

	void callback_dynamic_reconfigure(crazyflie_controller::crazyflie_estimatorConfig &config, uint32_t level)
		{

		if (level)
			{
			delay = config.delay;
			ROS_INFO_STREAM("Delay: " << delay << endl);
			ROS_INFO_STREAM("Delay config: " << config.delay << endl);
			}
		}

	Quaterniond euler2quatern(euler angle)
		{

		Quaterniond q;

		double cosPhi = cos(angle.phi*0.5);
		double sinPhi = sin(angle.phi*0.5);

		double cosTheta = cos(angle.theta*0.5);
		double sinTheta = sin(angle.theta*0.5);

		double cosPsi = cos(angle.psi*0.5);
		double sinPsi = sin(angle.psi*0.5);

		q.w() = cosPhi*cosTheta*cosPsi + sinPhi*sinTheta*sinPsi;
		q.x() = -(cosPsi*cosTheta*sinPhi - sinPsi*sinTheta*cosPhi);
		q.y() = -(cosPsi*sinTheta*cosPhi + sinPsi*cosTheta*sinPhi);
		q.z() = -(sinPsi*cosTheta*cosPhi - cosPsi*sinTheta*sinPhi);


		if(q.w() < 0)
		  {
		  q.w() = -q.w();
		  q.vec() = -q.vec();
		  }

		return q;
		}

	double linearVelocityLPF(
		std::vector <double> q_samples,
		std::vector <double> dq_samples,
		double Ts, double elapsed_time)
		{
		// digital low-pass filter considering Ts = 15 ms
		double dq = 0;
		if (elapsed_time > 1.0)
			dq = 0.3306*dq_samples[4] - 0.02732*dq_samples[3] + 35.7*q_samples[4] - 35.7*q_samples[3];
		else
			dq = (q_samples[4] - q_samples[3]) / Ts;
		return dq;
		}

	Vector3d estimateWordLinearVelocities(float dt, float delta)
		{

		Vector3d vearth;
		//estimte the velocity
		x_samples[0] = x_samples[1];
		x_samples[1] = x_samples[2];
		x_samples[2] = x_samples[3];
		x_samples[3] = x_samples[4];
		x_samples[4] = actual_x;
		y_samples[0] = y_samples[1];
		y_samples[1] = y_samples[2];
		y_samples[2] = y_samples[3];
		y_samples[3] = y_samples[4];
		y_samples[4] = actual_y;
		z_samples[0] = z_samples[1];
		z_samples[1] = z_samples[2];
		z_samples[2] = z_samples[3];
		z_samples[3] = z_samples[4];
		z_samples[4] = actual_z;

		vearth[0] = linearVelocityLPF(x_samples, vx_filter_samples, dt, delta);
		vearth[1] = linearVelocityLPF(y_samples, vy_filter_samples, dt, delta);
		vearth[2] = linearVelocityLPF(z_samples, vz_filter_samples, dt, delta);

		vx_filter_samples[0] = vx_filter_samples[1];
		vx_filter_samples[1] = vx_filter_samples[2];
		vx_filter_samples[2] = vx_filter_samples[3];
		vx_filter_samples[3] = vx_filter_samples[4];
		vx_filter_samples[4] = vearth[0];
		vy_filter_samples[0] = vy_filter_samples[1];
		vy_filter_samples[1] = vy_filter_samples[2];
		vy_filter_samples[2] = vy_filter_samples[3];
		vy_filter_samples[3] = vy_filter_samples[4];
		vy_filter_samples[4] = vearth[1];
		vz_filter_samples[0] = vz_filter_samples[1];
		vz_filter_samples[1] = vz_filter_samples[2];
		vz_filter_samples[2] = vz_filter_samples[3];
		vz_filter_samples[3] = vz_filter_samples[4];
		vz_filter_samples[4] = vearth[2];

		return vearth;
		}

	Vector3d rotateLinearVeloE2B(Quaterniond* q, Vector3d v_inertial)
		{
		 // This is the convertion between
		 // quaternion orientation to rotation matrix
		 // from EARTH to BODY (considering ZYX euler sequence)
		 Matrix3d Sq;
		 Vector3d vb;

	 	 double S11 = 2*(q->w()*q->w()+q->x()*q->x())-1;
	 	 double S12 = 2*(q->x()*q->y()+q->w()*q->z());
	 	 double S13 = 2*(q->x()*q->z()-q->w()*q->y());
	 	 double S21 = 2*(q->x()*q->y()-q->w()*q->z());
	 	 double S22 = 2*(q->w()*q->w()+q->y()*q->y())-1;
	 	 double S23 = 2*(q->y()*q->z()+q->w()*q->x());
	 	 double S31 = 2*(q->x()*q->z()+q->w()*q->y());
	 	 double S32 = 2*(q->y()*q->z()-q->w()*q->x());
	 	 double S33 = 2*(q->w()*q->w()+q->z()*q->z())-1;


		Sq << S11,S12,S13,
		      S21,S22,S23,
		      S31,S32,S33;

		vb = Sq*v_inertial;

		return vb;
		}

	double deg2Rad(double deg)
		{
		return deg / 180.0 * pi;
		}

	double rad2Deg(double rad)
		{
		return rad * 180.0 / pi;
		}

	void sensorfusion6Callback(const crazyflie_controller::GenericLogDataConstPtr& msg)
		{
			actual_qw = msg->values[0];
			actual_qx = msg->values[1];
			actual_qy = msg->values[2];
			actual_qz = msg->values[3];
		}

	void motorsCallback(const crazyflie_controller::PropellerSpeedsStampedPtr& msg)
		{
		// motors rpm from mpc solution
		acados_w1_prelatest = acados_w1_latest;
		acados_w2_prelatest = acados_w2_latest;
		acados_w3_prelatest = acados_w3_latest;
		acados_w4_prelatest = acados_w4_latest;

		acados_w1_latest = msg->w1;
		acados_w2_latest = msg->w2;
		acados_w3_latest = msg->w3;
		acados_w4_latest = msg->w4;

		}

	void actual_motorsCallback(const crazyflie_controller::GenericLogDataConstPtr& msg)
		{
		actual_m1 = msg->values[0];
		actual_m2 = msg->values[1];
		actual_m3 = msg->values[2];
		actual_m4 = msg->values[3];
		}

	void eRaptorCallback(const geometry_msgs::PoseStampedConstPtr& msg)
		{
		// Position of crazyflie marker
		actual_x = msg->pose.position.x;
		actual_y = msg->pose.position.y;
		actual_z = msg->pose.position.z;
		}

	void eulerCallback(const geometry_msgs::Vector3StampedPtr& msg)
		{
		// Euler angles
		actual_roll  =  msg->vector.x;
		actual_pitch =  -msg->vector.y;
		actual_yaw   =  msg->vector.z;

		crazyflie_controller::EulerAnglesStamped euler_msg;
		euler_msg.header 	= msg->header;
		euler_msg.roll 		= actual_roll;
		euler_msg.pitch 	= actual_pitch;
		euler_msg.yaw 		= actual_yaw;

		p_cf_euler.publish(euler_msg);
		}

	void imuCallback(const sensor_msgs::Imu::ConstPtr& msg)
		{
		// Angular rates w.r.t. body frame in rad/s
		actual_wx = msg->angular_velocity.x;
		actual_wy = msg->angular_velocity.y;
		actual_wz = msg->angular_velocity.z;
		}

	int krpm2pwm(double Krpm)
		{
		int pwm = ((Krpm*1000)-4070.3)/0.2685;
		return pwm;
		}

	void predictor(const ros::TimerEvent& e)
		{

		if(e.last_real.isZero())
		{
		t0 = e.current_real.toSec();
		}

		double dt = e.current_real.toSec() - e.last_real.toSec();

		// --- Position
		sim_acados_in.x0[xq] = actual_x;
		sim_acados_in.x0[yq] = actual_y;
		sim_acados_in.x0[zq] = actual_z;

		// --- Quaternions
		//
		// Get the euler angles from the onboard stabilizer
		euler eu;
		eu.phi 		= deg2Rad(actual_roll);
		eu.theta 	= deg2Rad(actual_pitch);
		eu.psi 		= deg2Rad(actual_yaw);

		// Convert IMU euler angles to quaternion
		Quaterniond q_imu = euler2quatern(eu);
		q_imu.normalize();

		// storing converted quaternions
		sim_acados_in.x0[qw] = q_imu.w();
		sim_acados_in.x0[qx] = q_imu.x();
		sim_acados_in.x0[qy] = q_imu.y();
		sim_acados_in.x0[qz] = q_imu.z();

		// -- inertial linear velocities
		Vector3d vi_mat;
		vi_mat = estimateWordLinearVelocities(dt,t0);

		// --- Body linear velocities
		Vector3d vb_mat;
		vb_mat = rotateLinearVeloE2B(&q_imu,vi_mat);

		// overwriting linear velocities in the body frame in state vector
		sim_acados_in.x0[vbx] = vb_mat[0];
		sim_acados_in.x0[vby] = vb_mat[1];
		sim_acados_in.x0[vbz] = vb_mat[2];

		// --- Body angular velocities
		sim_acados_in.x0[wx] = actual_wx;
		sim_acados_in.x0[wy] = actual_wy;
		sim_acados_in.x0[wz] = actual_wz;

		// set discretization time
		sim_in_set(crazyflie_sim_config, crazyflie_sim_dims, crazyflie_sim_in, "T", &delay);

		// ROS_INFO_STREAM("Delay: " << delay << endl);

		// set initial state
		sim_in_set(crazyflie_sim_config, crazyflie_sim_dims, crazyflie_sim_in, "x", sim_acados_in.x0);

		// set control
		sim_acados_in.u0[w1] = acados_w1_latest;
		sim_acados_in.u0[w2] = acados_w2_latest;
		sim_acados_in.u0[w3] = acados_w3_latest;
		sim_acados_in.u0[w4] = acados_w4_latest;

		sim_in_set(crazyflie_sim_config, crazyflie_sim_dims, crazyflie_sim_in, "u", sim_acados_in.u0);

		// solve
		sim_acados_status = crazyflie_acados_sim_solve();
		// ROS_INFO_STREAM("Sim solver : " << sim_acados_status << endl);

		// get and print output
		sim_out_get(crazyflie_sim_config, crazyflie_sim_dims, crazyflie_sim_out, "xn", sim_acados_out.xn);

		// assign output signals
		sim_acados_out.status = sim_acados_status;
		// sim_acados_out.cpu_time = (double)crazyflie_sim_out->total_time;

		// publish state
		crazyflie_controller::CrazyflieStateStamped crazyflie_state;

		crazyflie_state.header.stamp = ros::Time::now();

		/*crazyflie_state.pos.x    = sim_acados_in.x0[xq];
		crazyflie_state.pos.y    = sim_acados_in.x0[yq];
		crazyflie_state.pos.z    = sim_acados_in.x0[zq];
		crazyflie_state.vel.x    = sim_acados_in.x0[vbx];
		crazyflie_state.vel.y    = sim_acados_in.x0[vby];
		crazyflie_state.vel.z    = sim_acados_in.x0[vbz];
		crazyflie_state.quat.w   = sim_acados_in.x0[qw];
		crazyflie_state.quat.x   = sim_acados_in.x0[qx];
		crazyflie_state.quat.y   = sim_acados_in.x0[qy];
		crazyflie_state.quat.z   = sim_acados_in.x0[qz];
		crazyflie_state.rates.x  = sim_acados_in.x0[wx];
		crazyflie_state.rates.y  = sim_acados_in.x0[wy];
		crazyflie_state.rates.z  = sim_acados_in.x0[wz];*/

		crazyflie_state.pos.x    = sim_acados_out.xn[xq];
		crazyflie_state.pos.y    = sim_acados_out.xn[yq];
		crazyflie_state.pos.z    = sim_acados_out.xn[zq];
		crazyflie_state.vel.x    = sim_acados_out.xn[vbx];
		crazyflie_state.vel.y    = sim_acados_out.xn[vby];
		crazyflie_state.vel.z    = sim_acados_out.xn[vbz];
		crazyflie_state.quat.w   = sim_acados_out.xn[qw];
		crazyflie_state.quat.x   = sim_acados_out.xn[qx];
		crazyflie_state.quat.y   = sim_acados_out.xn[qy];
		crazyflie_state.quat.z   = sim_acados_out.xn[qz];
		crazyflie_state.rates.x  = sim_acados_out.xn[wx];
		crazyflie_state.rates.y  = sim_acados_out.xn[wy];
		crazyflie_state.rates.z  = sim_acados_out.xn[wz];

		p_cf_state.publish(crazyflie_state);

		}
	};

int main(int argc, char **argv)
{
	ros::init(argc, argv, "cf_estimator");
	ros::NodeHandle n("~");

	double frequency = 66.6;
	double delay;
	n.getParam("delay", delay);

	ESTIMATOR est(n, delay);
	est.run(frequency);

	return 0;
}
