#include <types.h>
#include <lib.h>
#include <synchprobs.h>
#include <synch.h>
#include <opt-A1.h>
#include <array.h>


typedef struct Vehicle {
	Direction origin;
	Direction destination;
} Vehicle;

static struct array* volatile vehicles;
static struct lock *intersection_lk;
static struct cv *intersection_cv;
static bool can_enter(Vehicle* v);
static bool right_turn(Vehicle* v);

//  Taken from traffic.c
static bool right_turn(Vehicle *v) {
	KASSERT(v != NULL);
	if (((v->origin == west) && (v->destination == south)) ||
		((v->origin == south) && (v->destination == east)) ||
		((v->origin == east) && (v->destination == north)) ||
		((v->origin == north) && (v->destination == west))) {
		return true;
	}
	return false;
}

// Compares v to all vehicles currently in intersection and determines
// if v can enter
static bool can_enter(Vehicle *v) {
	int size = array_num(vehicles);
	if (size == 0) {
		return true;
	}
	for (int i = 0; i < size; ++i) {
		Vehicle *compare = array_get(vehicles, i);
		if ((v->origin == compare->origin) ||
			((v->origin == compare->destination) && 
			(v->destination == compare->origin)) ||
			((v->destination != compare->destination) && 
			(right_turn(v) || right_turn(compare)))) {
				continue;
			} else {
				return false;
			}
	}
	return true;
}

/* 
 * The simulation driver will call this function once before starting
 * the simulation
 *
 * You can use it to initialize synchronization and other variables.
 * 
 */
void
intersection_sync_init(void)
{
	intersection_lk = lock_create("intersectionLK");
	intersection_cv = cv_create("intersectionCV");
	vehicles = array_create();
	KASSERT(intersection_lk != NULL && intersection_cv != NULL && vehicles != NULL);
}

/* 
 * The simulation driver will call this function once after
 * the simulation has finished
 *
 * You can use it to clean up any synchronization and other variables.
 *
 */
void
intersection_sync_cleanup(void)
{
	KASSERT(intersection_lk != NULL && intersection_cv != NULL && vehicles != NULL);
	lock_destroy(intersection_lk);
	cv_destroy(intersection_cv);
	array_destroy(vehicles);
}


/*
 * The simulation driver will call this function each time a vehicle
 * tries to enter the intersection, before it enters.
 * This function should cause the calling simulation thread 
 * to block until it is OK for the vehicle to enter the intersection.
 *
 * parameters:
 *    * origin: the Direction from which the vehicle is arriving
 *    * destination: the Direction in which the vehicle is trying to go
 *
 * return value: none
 */

void
intersection_before_entry(Direction origin, Direction destination) 
{
	lock_acquire(intersection_lk);
	Vehicle *v = kmalloc(sizeof(struct Vehicle));
	KASSERT(v != NULL);
	v->origin = origin;
	v->destination = destination;
	while (!can_enter(v)) {
		// while vehicle cannot enter intersection
		cv_wait(intersection_cv, intersection_lk);
	}
	// we still have lock
	array_add(vehicles, v, NULL);
	lock_release(intersection_lk);
}


/*
 * The simulation driver will call this function each time a vehicle
 * leaves the intersection.
 *
 * parameters:
 *    * origin: the Direction from which the vehicle arrived
 *    * destination: the Direction in which the vehicle is going
 *
 * return value: none
 */

void
intersection_after_exit(Direction origin, Direction destination) 
{
	lock_acquire(intersection_lk);
	int size = array_num(vehicles);
	for (int i = 0; i < size; ++i) {
		Vehicle *v = array_get(vehicles, i);
		if ((v->origin == origin) && (v->destination == destination)) {
			// match found, must delete
			array_remove(vehicles, i);
			break;
		}
	}
	// wake up other threads
	cv_broadcast(intersection_cv, intersection_lk);
	lock_release(intersection_lk);
}