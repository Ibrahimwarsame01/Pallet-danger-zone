#ifndef ZONE_CHECK_H
#define ZONE_CHECK_H

#include "../../include/common_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------------
 * Zone check (Role C).
 *
 * Fixed polygon defined at startup (from config). Point-in-polygon test uses
 * ray casting — works for convex or non-convex simple polygons.
 *
 * All coordinates are pixel coords in the ORIGINAL frame.
 * -------------------------------------------------------------------------- */

/* Single-point test. False if polygon has fewer than 3 vertices. */
bool zone_check_point_in_polygon(const Polygon *zone, Point p);

/* Cross-role contract from common_types.h:
 *   Returns true iff ANY of the given points lies inside the zone.
 *   Safe with count=0 or points=NULL (returns false).
 */
/* Declared in common_types.h to lock the interface across roles. */

#ifdef __cplusplus
}
#endif

#endif /* ZONE_CHECK_H */
