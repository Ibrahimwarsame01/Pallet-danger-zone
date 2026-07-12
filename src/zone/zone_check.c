/* -----------------------------------------------------------------------------
 * zone_check.c — Role C
 *
 * Point-in-polygon via ray casting (Jordan curve theorem).
 * Standard algorithm — see e.g. https://wrf.ecse.rpi.edu/Research/Short_Notes/pnpoly.html
 * -------------------------------------------------------------------------- */

#include "zone_check.h"

#include <stdio.h>

bool zone_check_point_in_polygon(const Polygon *zone, Point p)
{
    if (!zone || !zone->vertices || zone->count < 3) return false;

    bool inside = false;
    int n = zone->count;

    for (int i = 0, j = n - 1; i < n; j = i++) {
        int xi = zone->vertices[i].x, yi = zone->vertices[i].y;
        int xj = zone->vertices[j].x, yj = zone->vertices[j].y;

        /* Cast a horizontal ray to the right; count edges it crosses. */
        bool y_crosses = ((yi > p.y) != (yj > p.y));
        if (!y_crosses) continue;

        /* Intersection X at ray height (avoid divide-by-zero: yi != yj here). */
        double intersect_x =
            (double)(xj - xi) * (double)(p.y - yi) / (double)(yj - yi) + (double)xi;

        if ((double)p.x < intersect_x) inside = !inside;
    }
    return inside;
}

bool zone_check_is_breached(Polygon *zone, Point *points, int count)
{
    if (!zone || !points || count <= 0) return false;
    for (int i = 0; i < count; i++) {
        if (zone_check_point_in_polygon(zone, points[i])) return true;
    }
    return false;
}
