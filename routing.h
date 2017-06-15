#ifndef ROUTING_H
#define ROUTING_H

#include "mapping.h"

typedef struct
{
	int x;
	int y;
} route_xy_t;

typedef struct route_unit_t route_unit_t;

struct route_unit_t
{
	route_xy_t loc;
	int backmode;
	route_unit_t* prev;
	route_unit_t* next;
};

void clear_route(route_unit_t **route);
int search_route(world_t *w, route_unit_t **route, float start_ang, int start_x_mm, int start_y_mm, int end_x_mm, int end_y_mm);

#define MINIMAP_SIZE 768
#define MINIMAP_MIDDLE 384
extern uint8_t minimap[MINIMAP_SIZE][MINIMAP_SIZE];

int minimap_find_mapping_dir(float ang_now, int32_t* x, int32_t* y);

#endif
