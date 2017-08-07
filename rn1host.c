#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/select.h>
#include <errno.h>
#include <math.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "datatypes.h"
#include "hwdata.h"
#include "map_memdisk.h"
#include "mapping.h"
#include "uart.h"
#include "tcp_comm.h"
#include "tcp_parser.h"
#include "routing.h"
#include "utlist.h"
#include "tof3d.h"

#include "mcu_micronavi_docu.c"

tof3d_scan_t* get_tof3d(void);

int max_speedlim = 35;
int cur_speedlim = 35;

double subsec_timestamp()
{
	struct timespec spec;
	clock_gettime(CLOCK_MONOTONIC, &spec);

	return (double)spec.tv_sec + (double)spec.tv_nsec/1.0e9;
}

int mapping_on = 0;
int live_obstacle_checking_on = 1;
int pos_corr_id = 42;
#define INCR_POS_CORR_ID() {pos_corr_id++; if(pos_corr_id > 99) pos_corr_id = 0;}


int map_significance_mode = MAP_SEMISIGNIFICANT_IMGS | MAP_SIGNIFICANT_IMGS;

int motors_on = 1;

uint32_t robot_id = 0xacdcabba; // Hopefully unique identifier for the robot.

extern world_t world;
#define BUFLEN 2048

int32_t cur_ang, cur_x, cur_y;
double robot_pos_timestamp;
int32_t cur_compass_ang;
int compass_round_active;
int32_t dest_x, dest_y;

typedef struct
{
	int x;
	int y;
	int backmode;
	int take_next_early;
	int timeout;
} route_point_t;

#define THE_ROUTE_MAX 200
route_point_t the_route[THE_ROUTE_MAX];
int the_route_len = 0;

int do_follow_route = 0;
int lookaround_creep_reroute = 0;
int route_pos = 0;
int start_route = 0;
int id_cnt = 1;
int good_time_for_lidar_mapping = 0;

#define sq(x) ((x)*(x))

#define NUM_LATEST_LIDARS_FOR_ROUTING_START 7
lidar_scan_t* lidars_to_map_at_routing_start[NUM_LATEST_LIDARS_FOR_ROUTING_START];

int run_search()
{
	int32_t da, dx, dy;
	map_lidars(&world, NUM_LATEST_LIDARS_FOR_ROUTING_START, lidars_to_map_at_routing_start, &da, &dx, &dy);
	INCR_POS_CORR_ID();
	correct_robot_pos(da, dx, dy, pos_corr_id);

	route_unit_t *some_route = NULL;

	int ret = search_route(&world, &some_route, ANG32TORAD(cur_ang), cur_x, cur_y, dest_x, dest_y);

	route_unit_t *rt;
	int len = 0;
	DL_FOREACH(some_route, rt)
	{
//		if(rt->backmode)
//			printf(" REVERSE ");
//		else
//			printf("         ");

		int x_mm, y_mm;
		mm_from_unit_coords(rt->loc.x, rt->loc.y, &x_mm, &y_mm);					
//		printf("to %d,%d\n", x_mm, y_mm);

		the_route[len].x = x_mm; the_route[len].y = y_mm; the_route[len].backmode = rt->backmode;
		the_route[len].take_next_early = 100;
		len++;
		if(len >= THE_ROUTE_MAX)
			break;
	}

	for(int i = 0; i < len; i++)
	{
		if(i < len-1)
		{
			float dist = sqrt(sq(the_route[i].x-the_route[i+1].x) + sq(the_route[i].y-the_route[i+1].y));
			int new_early = dist/10;
			if(new_early < 50) new_early = 50;
			else if(new_early > 250) new_early = 250;
			the_route[i].take_next_early = new_early;
		}
	}

	the_route[len-1].take_next_early = 20;


	tcp_send_route(&some_route);

	if(some_route)
	{
		the_route_len = len;
		do_follow_route = 1;
		start_route = 1;
		route_pos = 0;
		id_cnt++; if(id_cnt > 7) id_cnt = 1;
	}
	else do_follow_route = 0;

	return ret;

}

static int maneuver_cnt = 0; // to prevent too many successive maneuver operations
void do_live_obstacle_checking()
{
	if(the_route[route_pos].backmode == 0)
	{
		int hitcnt = check_direct_route_non_turning_hitcnt_mm(cur_x, cur_y, the_route[route_pos].x, the_route[route_pos].y);

		if(hitcnt > 0 && maneuver_cnt < 3)
		{
			// See what happens if we steer left or right

			int best_hitcnt = 9999;
			int best_drift_idx = 0;
			int best_angle_idx = 0;
			int best_new_x = 0, best_new_y = 0;

			const int side_drifts[14] = {320,-320, 240,-240,200,-200,160,-160,120,-120,80,-80,40,-40};
			const float drift_angles[5] = {M_PI/4.0, M_PI/6.0, M_PI/8.0, M_PI/12.0, M_PI/16.0};

			for(int angle_idx=0; angle_idx<5; angle_idx++)
			{
				for(int drift_idx=0; drift_idx<14; drift_idx++)
				{
					int new_x, new_y;
					if(side_drifts[drift_idx] > 0)
					{
						new_x = cur_x + cos(ANG32TORAD(cur_ang)+drift_angles[angle_idx])*side_drifts[drift_idx];
						new_y = cur_y + sin(ANG32TORAD(cur_ang)+drift_angles[angle_idx])*side_drifts[drift_idx];
					}
					else
					{
						new_x = cur_x + cos(ANG32TORAD(cur_ang)-drift_angles[angle_idx])*(-1*side_drifts[drift_idx]);
						new_y = cur_y + sin(ANG32TORAD(cur_ang)-drift_angles[angle_idx])*(-1*side_drifts[drift_idx]);
					}
					int drifted_hitcnt = check_direct_route_hitcnt_mm(cur_ang, new_x, new_y, the_route[route_pos].x, the_route[route_pos].y);
					if(drifted_hitcnt <= best_hitcnt)
					{
						best_hitcnt = drifted_hitcnt;
						best_drift_idx = drift_idx;
						best_angle_idx = angle_idx;
						best_new_x = new_x; best_new_y = new_y;
					}
				}
			}

			if(best_hitcnt < hitcnt)
			{
				printf("!!!!!!!!!!   INFO: Steering is needed to maintain line-of-sight, hitcnt now = %d, optimum drift = %.1f degs, %d mm (hitcnt=%d), cur(%d,%d) to(%d,%d)\n", 
					hitcnt, RADTODEG(drift_angles[best_angle_idx]), side_drifts[best_drift_idx], best_hitcnt, cur_x, cur_y, best_new_x, best_new_y);

				// Do the steer
				id_cnt = 0; // id0 is reserved for special maneuvers during route following.
				move_to(best_new_x, best_new_y, 0, (id_cnt<<4) | ((route_pos)&0b1111), 12, 0);
				maneuver_cnt++;

			}
			else
			{
				printf("!!!!!!!!  INFO: Steering cannot help in improving line-of-sight.\n");
				if(hitcnt < 3)
				{
					printf("!!!!!!!!!!!  INFO: Direct line-of-sight to the next point has 1..2 obstacles, slowing down.\n");
					cur_speedlim = 12;
					limit_speed(cur_speedlim);
				}
				else
				{
					printf("!!!!!!!!!!!  INFO: Direct line-of-sight to the next point has disappeared! Trying to solve.\n");
					stop_movement();
					lookaround_creep_reroute = 1;
				}

			}
		}
	}
}

void route_fsm()
{
	static double timestamp;
	static int creep_cnt;

	if(lookaround_creep_reroute)
	{
		if(check_direct_route_non_turning_mm(cur_x, cur_y, the_route[route_pos].x, the_route[route_pos].y))
		{
			printf("INFO: Direct line-of-sight to the next waypoint, we are done, resuming following the route.\n");
			lookaround_creep_reroute = 0;
			do_follow_route = 1;
			id_cnt++; if(id_cnt > 7) id_cnt = 1;
			move_to(the_route[route_pos].x, the_route[route_pos].y, the_route[route_pos].backmode, (id_cnt<<4) | ((route_pos)&0b1111), cur_speedlim, 0);
		}
	}

	if(lookaround_creep_reroute == 1)
	{
		do_follow_route = 0;
		start_route = 0;

		printf("INFO: Lookaround, creep & reroute procedure started; backing off 50 mm.\n");
		turn_and_go_abs_rel(cur_ang, -50, 13, 1);
		timestamp = subsec_timestamp();
		lookaround_creep_reroute++;
	}
	else if(lookaround_creep_reroute == 2)
	{
		double stamp;
		if( (stamp=subsec_timestamp()) > timestamp+1.0)
		{
			int dx = the_route[route_pos].x - cur_x;
			int dy = the_route[route_pos].y - cur_y;
			float ang = atan2(dy, dx) /*<- ang to dest*/ - DEGTORAD(15.0);

			if(test_robot_turn_mm(cur_x, cur_y, ANG32TORAD(cur_ang),  ang))
			{
				printf("INFO: Can turn -15 deg, doing it.\n");
				turn_and_go_abs_rel(RADTOANG32(ang), 0, 13, 1);
			}
			else
			{
				printf("INFO: Can't turn -15 deg, wiggling a bit.\n");
				turn_and_go_abs_rel(cur_ang-5*ANG_1_DEG, 0, 13, 1);
			}
			timestamp = subsec_timestamp();
			lookaround_creep_reroute++;
		}
	}
	else if(lookaround_creep_reroute == 3)
	{
		double stamp;
		if( (stamp=subsec_timestamp()) > timestamp+1.0)
		{
			int dx = the_route[route_pos].x - cur_x;
			int dy = the_route[route_pos].y - cur_y;
			float ang = atan2(dy, dx) /*<- ang to dest*/ - DEGTORAD(30.0);

			if(test_robot_turn_mm(cur_x, cur_y, ANG32TORAD(cur_ang),  ang))
			{
				printf("INFO: Can turn -30 deg, doing it.\n");
				turn_and_go_abs_rel(RADTOANG32(ang), -20, 13, 1);
			}
			else
			{
				printf("INFO: Can't turn -30 deg, wiggling a bit.\n");
				turn_and_go_abs_rel(cur_ang-5*ANG_1_DEG, 0, 13, 1);
			}
			timestamp = subsec_timestamp();
			lookaround_creep_reroute++;
		}
	}
	else if(lookaround_creep_reroute == 4)
	{
		double stamp;
		if( (stamp=subsec_timestamp()) > timestamp+1.0)
		{
			int dx = the_route[route_pos].x - cur_x;
			int dy = the_route[route_pos].y - cur_y;
			float ang = atan2(dy, dx) /*<- ang to dest*/ + DEGTORAD(15.0);

			if(test_robot_turn_mm(cur_x, cur_y, ANG32TORAD(cur_ang),  ang))
			{
				printf("INFO: Can turn +15 deg, doing it.\n");
				turn_and_go_abs_rel(RADTOANG32(ang), 0, 13, 1);
			}
			else
			{
				printf("INFO: Can't turn +15 deg, wiggling a bit.\n");
				turn_and_go_abs_rel(cur_ang+10*ANG_1_DEG, 0, 13, 1);
			}
			timestamp = subsec_timestamp();
			lookaround_creep_reroute++;
		}
	}
	else if(lookaround_creep_reroute == 5)
	{
		double stamp;
		if( (stamp=subsec_timestamp()) > timestamp+1.0)
		{
			int dx = the_route[route_pos].x - cur_x;
			int dy = the_route[route_pos].y - cur_y;
			float ang = atan2(dy, dx) /*<- ang to dest*/ + DEGTORAD(30.0);

			if(test_robot_turn_mm(cur_x, cur_y, ANG32TORAD(cur_ang),  ang))
			{
				printf("INFO: Can turn +30 deg, doing it.\n");
				turn_and_go_abs_rel(RADTOANG32(ang), 0, 13, 1);
			}
			else
			{
				printf("INFO: Can't turn +30 deg, wiggling a bit.\n");
				turn_and_go_abs_rel(cur_ang+5*ANG_1_DEG, 0, 13, 1);
			}
			timestamp = subsec_timestamp();
			lookaround_creep_reroute++;
		}
	}
	else if(lookaround_creep_reroute == 6)
	{
		creep_cnt = 0;
		double stamp;
		if( (stamp=subsec_timestamp()) > timestamp+1.0)
		{
			int dx = the_route[route_pos].x - cur_x;
			int dy = the_route[route_pos].y - cur_y;
			float ang = atan2(dy, dx) /*<- ang to dest*/;

			if(test_robot_turn_mm(cur_x, cur_y, ANG32TORAD(cur_ang),  ang))
			{
				printf("INFO: Can turn towards the dest, doing it.\n");
				turn_and_go_abs_rel(RADTOANG32(ang), 50, 13, 1);
			}
			else
			{
				printf("INFO: Can't turn towards the dest, rerouting.\n");
				if(run_search() == 1)
				{
					printf("INFO: Routing failed in start, going to daiju mode for a while.\n");
					daiju_mode(1);
					lookaround_creep_reroute = 8;
				}
				else
				{
					printf("INFO: Routing succeeded, or failed later. Stopping lookaround, creep & reroute procedure.\n");
					lookaround_creep_reroute = 0;
				}
			}
			timestamp = subsec_timestamp();
			lookaround_creep_reroute++;
		}
	}
	else if(lookaround_creep_reroute == 7)
	{
		static double time_interval = 2.5;
		double stamp;
		if( (stamp=subsec_timestamp()) > timestamp+time_interval)
		{
			int dx = the_route[route_pos].x - cur_x;
			int dy = the_route[route_pos].y - cur_y;
			int dist = sqrt(sq(dx)+sq(dy));
			if(dist > 200 && creep_cnt < 5)
			{
				float ang = atan2(dy, dx) /*<- ang to dest*/;
				int creep_amount = 100;
				int dest_x = cur_x + cos(ang)*creep_amount;
				int dest_y = cur_y + sin(ang)*creep_amount;
				int hitcnt = check_direct_route_non_turning_hitcnt_mm(cur_x, cur_y, dest_x, dest_y);
				if(hitcnt < 1)
				{
					printf("INFO: Can creep %d mm towards the next waypoint, doing it\n", creep_amount);
					time_interval = 2.5;
					turn_and_go_abs_rel(RADTOANG32(ang) + ((creep_cnt&1)?(5*ANG_1_DEG):(-5*ANG_1_DEG)), creep_amount, 15, 1);
				}
				else
				{
					int careful_amount = 50;
					int imaginary_len = 200; // So that we choose to creep in a direction that doesn't have obstacles right around. (avoiding "from oja to allikko")
					const float creep_angs[7] = {DEGTORAD(-15), DEGTORAD(15), DEGTORAD(-10), DEGTORAD(10), DEGTORAD(-5), DEGTORAD(5), 0};
					int best_hitcnt = 999999;
					int best_creep_ang_idx = 0;
					for(int idx=0; idx < 7; idx++)
					{
						int imaginary_dest_x = cur_x + cos(ang)*(imaginary_len);
						int imaginary_dest_y = cur_y + sin(ang)*(imaginary_len);
						// Angles to be turned are small enough so that we'll just ignore the turn checking.
						int hitcnt = check_direct_route_non_turning_hitcnt_mm(cur_x, cur_y, imaginary_dest_x, imaginary_dest_y);
						if(hitcnt <= best_hitcnt)
						{
							best_hitcnt = hitcnt;
							best_creep_ang_idx = idx;
						}
					}

					if(best_hitcnt == 0) careful_amount = 100;
					else if(best_hitcnt == 1) careful_amount = 60;

					printf("INFO: Can't creep %d mm towards the next waypoint, turning & creeping %.1f deg, %d mm carefully to best possible direction (hitcnt=%d)\n",
						creep_amount, RADTODEG(creep_angs[best_creep_ang_idx]), careful_amount, best_hitcnt);
					time_interval = 4.0;
					turn_and_go_abs_rel(-1*RADTOANG32(creep_angs[best_creep_ang_idx]), careful_amount, 7, 1);
				}
				creep_cnt++;
			}
			else
			{
				printf("INFO: We have creeped enough (dist to waypoint=%d, creep_cnt=%d), no line of sight to the waypoint, trying to reroute\n",
					dist, creep_cnt);
				if(run_search() == 1)
				{
					printf("INFO: Routing failed in start, going to daiju mode for a while.\n");
					daiju_mode(1);
					lookaround_creep_reroute++;
				}
				else
				{
					printf("INFO: Routing succeeded, or failed later. Stopping lookaround, creep & reroute procedure.\n");
					lookaround_creep_reroute = 0;
				}

			}
			timestamp = subsec_timestamp();
		}
	}
	else if(lookaround_creep_reroute >= 8 && lookaround_creep_reroute < 12)
	{
		double stamp;
		if( (stamp=subsec_timestamp()) > timestamp+5.0)
		{
			printf("INFO: Daijued enough.\n");
			daiju_mode(0);
			if(run_search() == 1)
			{
				printf("INFO: Routing failed in start, going to daiju mode for a bit more...\n");
				daiju_mode(1);
				lookaround_creep_reroute++;
				timestamp = subsec_timestamp();
			}
			else
			{
				printf("INFO: Routing succeeded, or failed later. Stopping lookaround, creep & reroute procedure.\n");
				lookaround_creep_reroute = 0;
			}

		}
	}
	else if(lookaround_creep_reroute == 12)
	{
		printf("INFO: Giving up lookaround, creep & reroute procedure!\n");
		lookaround_creep_reroute = 0;
	}

	if(start_route)
	{
		printf("Start going id=%d!\n", id_cnt<<4);
		move_to(the_route[route_pos].x, the_route[route_pos].y, the_route[route_pos].backmode, (id_cnt<<4), cur_speedlim, 0);
		start_route = 0;
	}

	if(do_follow_route)
	{
		int id = cur_xymove.id;

		if(((id&0b1110000) == (id_cnt<<4)) && ((id&0b1111) == ((route_pos)&0b1111)))
		{
			if(cur_xymove.micronavi_stop_flags || cur_xymove.feedback_stop_flags)
			{
				printf("INFO: Micronavi STOP, entering lookaround_creep_reroute\n");
				lookaround_creep_reroute = 1;
			}
			else if(id_cnt == 0) // Zero id move is a special move during route following
			{
				if(cur_xymove.remaining < 30)
				{
					while(the_route[route_pos].backmode == 0 && route_pos < the_route_len-1)
					{
						if(check_direct_route_mm(cur_ang, cur_x, cur_y, the_route[route_pos+1].x, the_route[route_pos+1].y))
						{
							printf("!!!!!!!!!!!  INFO: Maneuver done; but skipping point (%d, %d), going directly to (%d, %d)\n", the_route[route_pos].x,
							       the_route[route_pos].y, the_route[route_pos+1].x, the_route[route_pos+1].y);
							route_pos++;
						}
						else
						{
							break;
						}
					}
					id_cnt = 1;
					printf("INFO: Maneuver done, redo the waypoint, id=%d!\n", (id_cnt<<4) | ((route_pos)&0b1111));
					move_to(the_route[route_pos].x, the_route[route_pos].y, the_route[route_pos].backmode, (id_cnt<<4) | ((route_pos)&0b1111), cur_speedlim, 0);
				}
			}
			else
			{
				if(cur_xymove.remaining < 250)
				{
					good_time_for_lidar_mapping = 1;
				}

				if(cur_xymove.remaining < the_route[route_pos].take_next_early)
				{
					maneuver_cnt = 0;
					if(route_pos < the_route_len-1)
					{
						route_pos++;

						// Check if we can skip some points:
						while(the_route[route_pos].backmode == 0 && route_pos < the_route_len-1)
						{
							if(check_direct_route_mm(cur_ang, cur_x, cur_y, the_route[route_pos+1].x, the_route[route_pos+1].y))
							{
								printf("!!!!!!!!!!!  INFO: skipping point (%d, %d), going directly to (%d, %d)\n", the_route[route_pos].x,
								       the_route[route_pos].y, the_route[route_pos+1].x, the_route[route_pos+1].y);
								route_pos++;
							}
							else
							{
								break;
							}
						}
						printf("INFO: Take the next, id=%d!\n", (id_cnt<<4) | ((route_pos)&0b1111));
						move_to(the_route[route_pos].x, the_route[route_pos].y, the_route[route_pos].backmode, (id_cnt<<4) | ((route_pos)&0b1111), cur_speedlim, 0);
					}
					else
					{
						printf("INFO: Done following the route.\n");
						do_follow_route = 0;
					}
				}
				else if(live_obstacle_checking_on)
				{
					// Check if obstacles have appeared in the map.

					static double prev_incr = 0.0;
					double stamp;
					if( (stamp=subsec_timestamp()) > prev_incr+0.20)
					{
						prev_incr = stamp;

						if(robot_pos_timestamp < stamp-0.30)
						{
							printf("INFO: Skipping live obstacle checking due to stale robot pos.\n");
						}
						else
						{
							do_live_obstacle_checking();
						}
					}
				}
			}
		}

	}

}

int32_t charger_ang;
int charger_fwd;
int charger_first_x, charger_first_y, charger_second_x, charger_second_y;
#define CHARGER_FIRST_DIST 1000
#define CHARGER_SECOND_DIST 500
#define CHARGER_THIRD_DIST  170

/*
void conf_charger_pos_pre()  // call when the robot is *in* the charger.
{
	int32_t da, dx, dy;
	map_lidars(&world, NUM_LATEST_LIDARS_FOR_ROUTING_START, lidars_to_map_at_routing_start, &da, &dx, &dy);
	INCR_POS_CORR_ID();
	correct_robot_pos(da, dx, dy, pos_corr_id);
}
*/

void save_robot_pos()
{
	FILE* f_cha = fopen("/home/hrst/rn1-host/robot_pos.txt", "w");
	if(f_cha)
	{
		fprintf(f_cha, "%d %d %d\n", cur_ang, cur_x, cur_y);
		fclose(f_cha);
	}
}

void retrieve_robot_pos()
{
	int32_t ang;
	int x; int y;
	FILE* f_cha = fopen("/home/hrst/rn1-host/robot_pos.txt", "r");
	if(f_cha)
	{
		fscanf(f_cha, "%d %d %d", &ang, &x, &y);
		fclose(f_cha);
		set_robot_pos(ang, x, y);
	}
}

void conf_charger_pos()  // call when the robot is *in* the charger.
{
	int32_t da, dx, dy;
	map_lidars(&world, NUM_LATEST_LIDARS_FOR_ROUTING_START, lidars_to_map_at_routing_start, &da, &dx, &dy);
	INCR_POS_CORR_ID();

	int32_t cha_ang = cur_ang-da; int cha_x = cur_x+dx; int cha_y = cur_y+dy;

	correct_robot_pos(da, dx, dy, pos_corr_id);

	printf("INFO: Set charger pos at ang=%d, x=%d, y=%d\n", cha_ang, cha_x, cha_y);
	charger_first_x = (float)cha_x - cos(ANG32TORAD(cha_ang))*(float)CHARGER_FIRST_DIST;
	charger_first_y = (float)cha_y - sin(ANG32TORAD(cha_ang))*(float)CHARGER_FIRST_DIST;	
	charger_second_x = (float)cha_x - cos(ANG32TORAD(cha_ang))*(float)CHARGER_SECOND_DIST;
	charger_second_y = (float)cha_y - sin(ANG32TORAD(cha_ang))*(float)CHARGER_SECOND_DIST;
	charger_fwd = CHARGER_SECOND_DIST-CHARGER_THIRD_DIST;
	charger_ang = cha_ang;

	FILE* f_cha = fopen("/home/hrst/rn1-host/charger_pos.txt", "w");
	if(f_cha)
	{
		fprintf(f_cha, "%d %d %d %d %d %d\n", charger_first_x, charger_first_y, charger_second_x, charger_second_y, charger_ang, charger_fwd);
		fclose(f_cha);
	}
}

void read_charger_pos()
{
	FILE* f_cha = fopen("/home/hrst/rn1-host/charger_pos.txt", "r");
	if(f_cha)
	{
		fscanf(f_cha, "%d %d %d %d %d %d", &charger_first_x, &charger_first_y, &charger_second_x, &charger_second_y, &charger_ang, &charger_fwd);
		fclose(f_cha);
		printf("Info: charger position retrieved from file: %d, %d --> %d, %d, ang=%d, fwd=%d\n", charger_first_x, charger_first_y, charger_second_x, charger_second_y, charger_ang, charger_fwd);
	}
}


void request_tof_quit(void);

void* main_thread()
{
	int find_charger_state = 0;
	if(init_uart())
	{
		fprintf(stderr, "uart initialization failed.\n");
		return NULL;
	}

	if(init_tcp_comm())
	{
		fprintf(stderr, "TCP communication initialization failed.\n");
		return NULL;
	}

	srand(time(NULL));

	send_keepalive();
	daiju_mode(0);
	correct_robot_pos(0,0,0, pos_corr_id); // To set the pos_corr_id.
	turn_and_go_rel_rel(-5*ANG_1_DEG, 0, 25, 1);
	sleep(1);
	send_keepalive();
	turn_and_go_rel_rel(10*ANG_1_DEG, 0, 25, 1);
	sleep(1);
	send_keepalive();
	turn_and_go_rel_rel(-5*ANG_1_DEG, 50, 25, 1);
	sleep(1);
	send_keepalive();
	turn_and_go_rel_rel(0, -50, 25, 1);
	sleep(1);

	double chafind_timestamp = 0.0;
	int lidar_ignore_over = 0;
	while(1)
	{
		// Calculate fd_set size (biggest fd+1)
		int fds_size = uart;
		if(tcp_listener_sock > fds_size) fds_size = tcp_listener_sock;
		if(tcp_client_sock > fds_size) fds_size = tcp_client_sock;
		if(STDIN_FILENO > fds_size) fds_size = STDIN_FILENO;
		fds_size+=1;


		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(uart, &fds);
		FD_SET(STDIN_FILENO, &fds);
		FD_SET(tcp_listener_sock, &fds);
		if(tcp_client_sock >= 0)
			FD_SET(tcp_client_sock, &fds);

		struct timeval select_time = {0, 200};

		if(select(fds_size, &fds, NULL, NULL, &select_time) < 0)
		{
			fprintf(stderr, "select() error %d", errno);
			return NULL;
		}

		if(FD_ISSET(STDIN_FILENO, &fds))
		{
			int cmd = fgetc(stdin);
			if(cmd == 'q')
				break;
			if(cmd == 'S')
			{
				save_robot_pos();
			}
			if(cmd == 's')
			{
				retrieve_robot_pos();
			}
			if(cmd == 'c')
			{
				printf("Starting automapping from compass round.\n");
				routing_set_world(&world);
				start_automapping_from_compass();
			}
			if(cmd == 'a')
			{
				printf("Starting automapping, skipping compass round.\n");
				routing_set_world(&world);
				start_automapping_skip_compass();
			}
			if(cmd == 'w')
			{
				printf("Stopping automapping.\n");
				stop_automapping();
			}
			if(cmd == '0')
			{
				set_robot_pos(0,0,0);
			}
			if(cmd == 'm')
			{
				if(mapping_on)
				{
					mapping_on = 0;
					printf("Turned mapping off.\n");
				}
				else
				{
					mapping_on = 1;
					printf("Turned mapping on.\n");
				}
			}
			if(cmd == 'M')
			{
				mapping_on = 2;
				printf("Turned mapping to fast mode.\n");
			}
//			if(cmd == 'K')
//			{
//				conf_charger_pos_pre();
//			}
			if(cmd == 'L')
			{
				conf_charger_pos();
			}
			if(cmd == 'l')
			{
				read_charger_pos();
				find_charger_state = 1;
			}
			if(cmd == 'v')
			{
				if(motors_on)
				{
					motors_on = 0;
					printf("Robot is free to move manually.\n");
				}
				else
				{
					motors_on = 1;
					printf("Robot motors enabled again.\n");
				}
			}
			if(cmd == 'd')
			{
				dbg_test();
			}

		}

		if(FD_ISSET(uart, &fds))
		{
			handle_uart();
		}

		if(tcp_client_sock >= 0 && FD_ISSET(tcp_client_sock, &fds))
		{
			int ret = handle_tcp_client();
			if(ret == TCP_CR_DEST_MID)
			{
				motors_on = 1;
				daiju_mode(0);

				printf("  ---> DEST params: X=%d Y=%d backmode=%d\n", msg_cr_dest.x, msg_cr_dest.y, msg_cr_dest.backmode);
				move_to(msg_cr_dest.x, msg_cr_dest.y, msg_cr_dest.backmode, 0, cur_speedlim, 1);
				find_charger_state = 0;
				lookaround_creep_reroute = 0;
				do_follow_route = 0;
			}
			else if(ret == TCP_CR_ROUTE_MID)
			{
				printf("  ---> ROUTE params: X=%d Y=%d dummy=%d\n", msg_cr_route.x, msg_cr_route.y, msg_cr_route.dummy);

				dest_x = msg_cr_route.x; dest_y = msg_cr_route.y;

				motors_on = 1;
				daiju_mode(0);
				find_charger_state = 0;

				if(run_search() == 1)
				{
					printf("INFO: Routing fails in the start, daijuing for a while to get a better position.\n");
					daiju_mode(1);
					printf("INFO: (Please retry after some time.)\n");
				}

			}
			else if(ret == TCP_CR_CHARGE_MID)
			{
				read_charger_pos();
				find_charger_state = 1;
			}
			else if(ret == TCP_CR_MODE_MID)
			{
				printf("INFO: Request for MODE %d\n", msg_cr_mode.mode);
				switch(msg_cr_mode.mode)
				{
					case 0:
					{
						motors_on = 1;
						daiju_mode(0);
						stop_automapping();
						mapping_on = 0;
					} break;

					case 1:
					{
						motors_on = 1;
						daiju_mode(0);
						stop_automapping();
						find_charger_state = 0;
						lookaround_creep_reroute = 0;
						do_follow_route = 0;
						mapping_on = 1;

					} break;

					case 2:
					{
						motors_on = 1;
						daiju_mode(0);
						routing_set_world(&world);
						start_automapping_skip_compass();
						mapping_on = 1;
					} break;

					case 3:
					{
						motors_on = 1;
						daiju_mode(0);
						routing_set_world(&world);
						start_automapping_from_compass();
						mapping_on = 1;
					} break;

					case 4:
					{
						stop_automapping();
						find_charger_state = 0;
						lookaround_creep_reroute = 0;
						do_follow_route = 0;
						motors_on = 1;
						daiju_mode(1);
						mapping_on = 0;
					} break;

					case 5:
					{
						stop_automapping();
						find_charger_state = 0;
						lookaround_creep_reroute = 0;
						do_follow_route = 0;
						motors_on = 0;
						mapping_on = 1;
					} break;

					case 6:
					{
						stop_automapping();
						find_charger_state = 0;
						lookaround_creep_reroute = 0;
						do_follow_route = 0;
						motors_on = 0;
						mapping_on = 0;
					} break;

					case 7:
					{
						conf_charger_pos();
					} break;

					case 8:
					{
						stop_automapping();
						find_charger_state = 0;
						lookaround_creep_reroute = 0;
						do_follow_route = 0;
						stop_movement();
					} break;

					default: break;
				}
			}
			else if(ret == TCP_CR_MANU_MID)
			{
				#define MANU_FWD   10
				#define MANU_BACK  11
				#define MANU_LEFT  12
				#define MANU_RIGHT 13
				stop_automapping();
				daiju_mode(0);
				motors_on = 1;
				printf("INFO: Manual OP %d\n", msg_cr_manu.op);
				switch(msg_cr_manu.op)
				{
					case MANU_FWD:
						turn_and_go_abs_rel(cur_ang, 100, 10, 1);
					break;
					case MANU_BACK:
						turn_and_go_abs_rel(cur_ang, -100, 10, 1);
					break;
					case MANU_LEFT:
						turn_and_go_abs_rel(cur_ang-10*ANG_1_DEG, 0, 10, 1);
					break;
					case MANU_RIGHT:
						turn_and_go_abs_rel(cur_ang+10*ANG_1_DEG, 0, 10, 1);
					break;
					default:
					break;
				}
			}		
		}

		if(FD_ISSET(tcp_listener_sock, &fds))
		{
			handle_tcp_listener();
		}

//		static int prev_compass_ang = 0;

//		if(cur_compass_ang != prev_compass_ang)
//		{
//			prev_compass_ang = cur_compass_ang;
//			printf("INFO: Compass ang=%.1f deg\n", ANG32TOFDEG(cur_compass_ang));
//		}

		static int micronavi_stop_flags_printed = 0;

		if(cur_xymove.micronavi_stop_flags)
		{
			if(!micronavi_stop_flags_printed)
			{
				micronavi_stop_flags_printed = 1;
				printf("INFO: MCU-level micronavigation reported a stop. Stop reason flags:\n");
				for(int i=0; i<32; i++)
				{
					if(cur_xymove.micronavi_stop_flags&(1UL<<i))
					{
						printf("bit %2d: %s\n", i, MCU_NAVI_STOP_NAMES[i]);
					}
				}

				printf("Actions taken during stop condition:\n");
				for(int i=0; i<32; i++)
				{
					if(cur_xymove.micronavi_action_flags&(1UL<<i))
					{
						printf("bit %2d: %s\n", i, MCU_NAVI_ACTION_NAMES[i]);
					}
				}

				printf("\n");
			}
		}
		else
			micronavi_stop_flags_printed = 0;

		static int feedback_stop_flags_processed = 0;

		if(cur_xymove.feedback_stop_flags)
		{
			if(!feedback_stop_flags_processed)
			{
				feedback_stop_flags_processed = 1;
				int stop_reason = cur_xymove.feedback_stop_flags;
				printf("INFO: Feedback module reported: %s\n", MCU_FEEDBACK_COLLISION_NAMES[stop_reason]);
				if(mapping_on) map_collision_obstacle(&world, cur_ang, cur_x, cur_y, stop_reason, cur_xymove.stop_xcel_vector_valid,
					cur_xymove.stop_xcel_vector_ang_rad);
			}
		}
		else
			feedback_stop_flags_processed = 0;

		if(find_charger_state < 4)
			live_obstacle_checking_on = 1;
		else
			live_obstacle_checking_on = 0;

		if(find_charger_state == 1)
		{
			dest_x = charger_first_x; dest_y = charger_first_y;

			printf("Searching dest_x=%d  dest_y=%d\n", dest_x, dest_y);

			motors_on = 1;
			daiju_mode(0);
			if(run_search() != 0)
			{
				printf("Finding charger (first point) failed.\n");
				find_charger_state = 0;
			}
			else
				find_charger_state++;
		}
		else if(find_charger_state == 2)
		{
			if(!do_follow_route)
			{
				if(sq(cur_x-charger_first_x) + sq(cur_y-charger_first_y) > sq(300))
				{
					printf("INFO: We are not at the first charger point, trying again.\n");
					find_charger_state = 1;
				}
				else
				{
					printf("INFO: At first charger point, turning for charger.\n");
					turn_and_go_abs_rel(charger_ang, 0, 23, 1);
					find_charger_state++;
					chafind_timestamp = subsec_timestamp();

				}
			}
		}
		else if(find_charger_state == 3)
		{
			double stamp;
			if( (stamp=subsec_timestamp()) > chafind_timestamp+3.0)
			{
				chafind_timestamp = stamp;

				printf("INFO: Turned at first charger point, mapping lidars for exact pos.\n");

				int32_t da, dx, dy;
				map_lidars(&world, NUM_LATEST_LIDARS_FOR_ROUTING_START, lidars_to_map_at_routing_start, &da, &dx, &dy);
				INCR_POS_CORR_ID();
				correct_robot_pos(da, dx, dy, pos_corr_id);
				lidar_ignore_over = 0;
				find_charger_state++;
			}
		}
		else if(find_charger_state == 4)
		{
			if(lidar_ignore_over && subsec_timestamp() > chafind_timestamp+4.0)
			{
				printf("INFO: Going to second charger point.\n");
				move_to(charger_second_x, charger_second_y, 0, 0x7f, 20, 1);
				find_charger_state++;
			}
		}
		else if(find_charger_state == 5)
		{
			if(cur_xymove.id == 0x7f && cur_xymove.remaining < 10)
			{
				if(sq(cur_x-charger_second_x) + sq(cur_y-charger_second_y) > sq(180))
				{
					printf("INFO: We are not at the second charger point, trying again.\n");
					find_charger_state = 1;
				}
				else
				{
					turn_and_go_abs_rel(charger_ang, charger_fwd, 20, 1);
					chafind_timestamp = subsec_timestamp();
					find_charger_state++;
				}
			}
		}
		else if(find_charger_state == 6)
		{
			double stamp;
			if( (stamp=subsec_timestamp()) > chafind_timestamp+3.0)
			{
				chafind_timestamp = stamp;
				turn_and_go_abs_rel(charger_ang, 0, 23, 1);
				find_charger_state++;
			}
		}
		else if(find_charger_state == 7)
		{
			if(subsec_timestamp() > chafind_timestamp+1.5)
			{
				printf("INFO: Requesting charger mount.\n");
				hw_find_charger();
				find_charger_state = 0;
			}
		}

		route_fsm();
		autofsm();

		{
			static double prev_incr = 0.0;
			double stamp;
			if( (stamp=subsec_timestamp()) > prev_incr+0.15)
			{
				prev_incr = stamp;
				extern int32_t tof3d_obstacle_levels[3];
				extern pthread_mutex_t cur_pos_mutex;
				int obstacle_levels[3];
				pthread_mutex_lock(&cur_pos_mutex);
				obstacle_levels[0] = tof3d_obstacle_levels[0];
				obstacle_levels[1] = tof3d_obstacle_levels[0];
				obstacle_levels[2] = tof3d_obstacle_levels[0];
				pthread_mutex_unlock(&cur_pos_mutex);

				if(obstacle_levels[2] > 100)
				{
					if(cur_speedlim > 12)
					{
						cur_speedlim = 12;
						limit_speed(cur_speedlim);
					}
				}
				else if(obstacle_levels[2] > 7)
				{
					if(cur_speedlim > 16)
					{
						cur_speedlim = 16;
						limit_speed(cur_speedlim);
					}
				}
				else if(obstacle_levels[1] > 70)
				{
					if(cur_speedlim > 22)
					{
						cur_speedlim = 22;
						limit_speed(cur_speedlim);
					}
				}
				else if(obstacle_levels[1] > 7)
				{
					if(cur_speedlim > 28)
					{
						cur_speedlim = 28;
						limit_speed(cur_speedlim);
					}
				}
				else
				{
					if(cur_speedlim < max_speedlim)
					{
						cur_speedlim++;
						limit_speed(cur_speedlim);
					}
				}
			}
		}

		tof3d_scan_t *p_tof;
		if( (p_tof = get_tof3d()) )
		{

			if(tcp_client_sock >= 0)
			{
				static int hmap_cnt = 0;
				hmap_cnt++;

				if(hmap_cnt >= 15)
				{
//					printf("Send hmap\n");
					tcp_send_hmap(TOF3D_HMAP_XSPOTS, TOF3D_HMAP_YSPOTS, cur_ang, cur_x, cur_y, TOF3D_HMAP_SPOT_SIZE, p_tof->objmap);
//					printf("Done\n");
					hmap_cnt = 0;
				}
			}

			static int32_t prev_x, prev_y, prev_ang;

			if(mapping_on && (prev_x != p_tof->robot_pos.x || prev_y != p_tof->robot_pos.y || prev_ang != p_tof->robot_pos.ang))
			{
				prev_x = p_tof->robot_pos.x; prev_y = p_tof->robot_pos.y; prev_ang = p_tof->robot_pos.ang;

				static int n_tofs_to_map = 0;
				static tof3d_scan_t* tofs_to_map[20];

				tofs_to_map[n_tofs_to_map] = p_tof;
				n_tofs_to_map++;

				if(n_tofs_to_map >= 10)
				{
					int32_t mid_x, mid_y;
					map_3dtof(&world, n_tofs_to_map, tofs_to_map, &mid_x, &mid_y);

					if(do_follow_route)
					{
						int px, py, ox, oy;
						page_coords(mid_x, mid_y, &px, &py, &ox, &oy);

						for(int ix=-1; ix<=1; ix++)
						{
							for(int iy=-1; iy<=1; iy++)
							{
								gen_routing_page(&world, px+ix, py+iy, 0);
							}
						}
					}

					n_tofs_to_map = 0;
				}

			}

		}

		lidar_scan_t* p_lid;

		if( (p_lid = get_significant_lidar()) || (p_lid = get_basic_lidar()) )
		{
			static int hwdbg_cnt = 0;
			hwdbg_cnt++;
			if(hwdbg_cnt > 4)
			{
				if(tcp_client_sock >= 0) tcp_send_hwdbg(hwdbg);
				hwdbg_cnt = 0;
			}

			static int lidar_send_cnt = 0;
			lidar_send_cnt++;
			if(lidar_send_cnt > 8)
			{
				if(tcp_client_sock >= 0) tcp_send_lidar(p_lid);
				lidar_send_cnt = 0;
			}

			static int lidar_ignore_cnt = 0;

			if(p_lid->id != pos_corr_id)
			{

				lidar_ignore_cnt++;

//				if(p_lid->significant_for_mapping) 
//				printf("INFO: Ignoring lidar scan with id=%d (significance=%d).\n", p_lid->id, p_lid->significant_for_mapping);

				if(lidar_ignore_cnt > 50)
				{
					lidar_ignore_cnt = 0;
					printf("WARN: lidar id was stuck, fixing...\n");
					INCR_POS_CORR_ID();
					correct_robot_pos(0, 0, 0, pos_corr_id);

				}
			}
			else
			{
				lidar_ignore_cnt = 0;
				lidar_ignore_over = 1;

				int idx_x, idx_y, offs_x, offs_y;
	//			printf("INFO: Got lidar scan.\n");

				static int curpos_send_cnt = 0;
				curpos_send_cnt++;
				if(curpos_send_cnt > 2)
				{
					if(tcp_client_sock >= 0)
					{
						msg_rc_pos.ang = cur_ang>>16;
						msg_rc_pos.x = cur_x;
						msg_rc_pos.y = cur_y;
						tcp_send_msg(&msgmeta_rc_pos, &msg_rc_pos);
					}
					curpos_send_cnt = 0;
				}

				page_coords(p_lid->robot_pos.x, p_lid->robot_pos.y, &idx_x, &idx_y, &offs_x, &offs_y);
				load_9pages(&world, idx_x, idx_y);

				if(mapping_on)
				{
					// Clear any walls and items within the robot:
					clear_within_robot(&world, p_lid->robot_pos);
				}


				// Keep a pointer list of a few latest lidars; significant or insignificant will do.
				// This list is used to do last-second mapping before routing, to get good starting position.
				for(int i = NUM_LATEST_LIDARS_FOR_ROUTING_START-1; i >= 1; i--)
				{
					lidars_to_map_at_routing_start[i] = lidars_to_map_at_routing_start[i-1];
				}
				lidars_to_map_at_routing_start[0] = p_lid;
				if(p_lid->significant_for_mapping & map_significance_mode)
				{
//					lidar_send_cnt = 0;
//					if(tcp_client_sock >= 0) tcp_send_lidar(p_lid);

					static int n_lidars_to_map = 0;
					static lidar_scan_t* lidars_to_map[20];
					if(mapping_on)
					{
						if(p_lid->is_invalid)
						{
							if(n_lidars_to_map < 8)
							{
								printf("INFO: Got DISTORTED significant lidar scan, have too few lidars -> mapping queue reset\n");
								n_lidars_to_map = 0;
							}
							else
							{
								printf("INFO: Got DISTORTED significant lidar scan, running mapping early on previous images\n");
								int32_t da, dx, dy;
								prevent_3dtoffing();
								map_lidars(&world, n_lidars_to_map, lidars_to_map, &da, &dx, &dy);
								INCR_POS_CORR_ID();
								correct_robot_pos(da, dx, dy, pos_corr_id);

								n_lidars_to_map = 0;
							}
						}
						else
						{
//							printf("INFO: Got significant(%d) lidar scan, adding to the mapping queue(%d).\n", p_lid->significant_for_mapping, n_lidars_to_map);
							lidars_to_map[n_lidars_to_map] = p_lid;

							n_lidars_to_map++;
							if((good_time_for_lidar_mapping && n_lidars_to_map > 8) || n_lidars_to_map > 15)
							{
								if(good_time_for_lidar_mapping) good_time_for_lidar_mapping = 0;
								int32_t da, dx, dy;
								prevent_3dtoffing();
								map_lidars(&world, n_lidars_to_map, lidars_to_map, &da, &dx, &dy);
								INCR_POS_CORR_ID();
								correct_robot_pos(da, dx, dy, pos_corr_id);
								n_lidars_to_map = 0;
							}
						}
					}
					else
					{
						n_lidars_to_map = 0;
					}

				}

			}

			if(motors_on)
				send_keepalive();
			else
				release_motors();
		}

		sonar_scan_t* p_son;
		if( (p_son = get_sonar()) )
		{
			static int sonar_send_cnt = 0;
			sonar_send_cnt++;
			if(sonar_send_cnt > 8)
			{
				if(tcp_client_sock >= 0) tcp_send_sonar(p_son);
				sonar_send_cnt = 0;
			}
//			if(mapping_on)
//				map_sonar(&world, p_son);
		}

		static double prev_sync = 0;
		double stamp;

		double write_interval = 30.0;
		if(tcp_client_sock >= 0)
			write_interval = 7.0;

		if( (stamp=subsec_timestamp()) > prev_sync+write_interval)
		{
			prev_sync = stamp;

			int idx_x, idx_y, offs_x, offs_y;
			page_coords(cur_x, cur_y, &idx_x, &idx_y, &offs_x, &offs_y);

			// Do some "garbage collection" by disk-syncing and deallocating far-away map pages.
			unload_map_pages(&world, idx_x, idx_y);

			// Sync all changed map pages to disk
			if(save_map_pages(&world))
			{
				if(tcp_client_sock >= 0) tcp_send_sync_request();
			}
			if(tcp_client_sock >= 0) tcp_send_battery();

			static int automap_started = 0;
			if(!automap_started)
			{
				automap_started = 1;
//				start_automapping_from_compass();
			}

			fflush(stdout); // syncs log file.

		}
	}


	request_tof_quit();

	return NULL;
}



void* start_tof(void*);

int main(int argc, char** argv)
{
	pthread_t thread_main, thread_tof;

	uint8_t calib_tof = 0;
	if(argc == 2 && argv[1][0] == 'c')
		calib_tof = 1;

	int ret;

	if(!calib_tof)
	{
		if( (ret = pthread_create(&thread_main, NULL, main_thread, NULL)) )
		{
			printf("ERROR: main thread creation, ret = %d\n", ret);
			return -1;
		}
	}
	if( (ret = pthread_create(&thread_tof, NULL, start_tof, (void*)&calib_tof)) )
	{
		printf("ERROR: tof3d thread creation, ret = %d\n", ret);
		return -1;
	}

	if(!calib_tof)
		pthread_join(thread_main, NULL);

	pthread_join(thread_tof, NULL);

	return 0;
}
