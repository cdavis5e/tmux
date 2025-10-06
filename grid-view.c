/* $OpenBSD$ */

/*
 * Copyright (c) 2008 Nicholas Marriott <nicholas.marriott@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <string.h>

#include "tmux.h"

/*
 * Grid view functions. These work using coordinates relative to the visible
 * screen area.
 */

#define grid_view_x(gd, x) (x)
#define grid_view_y(gd, y) ((gd)->hsize + (y))

/* Get cell. */
void
grid_view_get_cell(struct grid *gd, u_int px, u_int py, struct grid_cell *gc)
{
	grid_get_cell(gd, grid_view_x(gd, px), grid_view_y(gd, py), gc);
}

/* Set cell. */
void
grid_view_set_cell(struct grid *gd, u_int px, u_int py,
    const struct grid_cell *gc)
{
	grid_set_cell(gd, grid_view_x(gd, px), grid_view_y(gd, py), gc);
}

/* Set padding. */
void
grid_view_set_padding(struct grid *gd, u_int px, u_int py)
{
	grid_set_padding(gd, grid_view_x(gd, px), grid_view_y(gd, py));
}

/* Set cells. */
void
grid_view_set_cells(struct grid *gd, u_int px, u_int py,
    const struct grid_cell *gc, const char *s, size_t slen)
{
	grid_set_cells(gd, grid_view_x(gd, px), grid_view_y(gd, py), gc, s,
	    slen);
}

/* Clear into history. */
void
grid_view_clear_history(struct grid *gd, u_int bg)
{
	struct grid_line	*gl;
	u_int			 yy, last;

	/* Find the last used line. */
	last = 0;
	for (yy = 0; yy < gd->sy; yy++) {
		gl = grid_get_line(gd, grid_view_y(gd, yy));
		if (gl->cellused != 0)
			last = yy + 1;
	}
	if (last == 0) {
		grid_view_clear(gd, 0, 0, gd->sx, gd->sy, bg);
		return;
	}

	/* Scroll the lines into the history. */
	for (yy = 0; yy < last; yy++) {
		grid_collect_history(gd);
		grid_scroll_history(gd, bg);
	}
	if (last < gd->sy)
		grid_view_clear(gd, 0, 0, gd->sx, gd->sy - last, bg);
	gd->hscrolled = 0;
}

/* Clear area. */
void
grid_view_clear(struct grid *gd, u_int px, u_int py, u_int nx, u_int ny,
    u_int bg)
{
	px = grid_view_x(gd, px);
	py = grid_view_y(gd, py);

	grid_clear(gd, px, py, nx, ny, bg);
}

/* Scroll region up. */
void
grid_view_scroll_region_up(struct grid *gd, u_int rupper, u_int rlower,
    u_int rleft, u_int rright, u_int bg)
{
	if (gd->flags & GRID_HISTORY) {
		grid_collect_history(gd);
		if (rupper == 0 && rlower == gd->sy - 1)
			grid_scroll_history(gd, bg);
		else {
			rupper = grid_view_y(gd, rupper);
			rlower = grid_view_y(gd, rlower);
			grid_scroll_history_region(gd, rupper, rlower, bg);
		}
	} else {
		rupper = grid_view_y(gd, rupper);
		rlower = grid_view_y(gd, rlower);
		rleft = grid_view_x(gd, rleft);
		rright = grid_view_x(gd, rright);
		grid_move_rect(gd, rleft, rupper, rleft, rupper + 1,
		    rright - rleft + 1, rlower - rupper, bg);
	}
}

/* Scroll region down. */
void
grid_view_scroll_region_down(struct grid *gd, u_int rupper, u_int rlower,
    u_int rleft, u_int rright, u_int bg)
{
	rupper = grid_view_y(gd, rupper);
	rlower = grid_view_y(gd, rlower);
	rleft = grid_view_x(gd, rleft);
	rright = grid_view_x(gd, rright);

	grid_move_rect(gd, rleft, rupper + 1, rleft, rupper, rright - rleft + 1,
	    rlower - rupper, bg);
}

/* Scroll region left. */
void
grid_view_scroll_region_left(struct grid *gd, u_int rupper, u_int rlower,
    u_int rleft, u_int rright, u_int bg)
{
	rupper = grid_view_y(gd, rupper);
	rlower = grid_view_y(gd, rlower);
	rleft = grid_view_x(gd, rleft);
	rright = grid_view_x(gd, rright);

	grid_move_rect(gd, rleft, rupper, rleft + 1, rupper, rright - rleft,
	    rlower - rupper + 1, bg);
}

/* Scroll region right. */
void
grid_view_scroll_region_right(struct grid *gd, u_int rupper, u_int rlower,
    u_int rleft, u_int rright, u_int bg)
{
	rupper = grid_view_y(gd, rupper);
	rlower = grid_view_y(gd, rlower);
	rleft = grid_view_x(gd, rleft);
	rright = grid_view_x(gd, rright);

	grid_move_rect(gd, rleft + 1, rupper, rleft, rupper, rright - rleft,
	    rlower - rupper + 1, bg);
}

/* Insert lines. */
void
grid_view_insert_lines(struct grid *gd, u_int py, u_int ny, u_int bg)
{
	u_int	sy;

	py = grid_view_y(gd, py);

	sy = grid_view_y(gd, gd->sy);

	grid_move_lines(gd, py + ny, py, sy - py - ny, bg);
}

/* Insert lines in region. */
void
grid_view_insert_lines_region(struct grid *gd, u_int rlower, u_int py,
    u_int ny, u_int rleft, u_int rright, u_int bg)
{
	u_int	ny2, nx;

	rlower = grid_view_y(gd, rlower);
	rleft = grid_view_x(gd, rleft);
	rright = grid_view_x(gd, rright);

	py = grid_view_y(gd, py);

	nx = rright - rleft + 1;
	ny2 = rlower + 1 - py - ny;
	grid_move_rect(gd, rleft, rlower + 1 - ny2, rleft, py, nx, ny2, bg);
	grid_clear(gd, rleft, py + ny2, nx, ny - ny2, bg);
}

/* Delete lines. */
void
grid_view_delete_lines(struct grid *gd, u_int py, u_int ny, u_int bg)
{
	u_int	sy;

	py = grid_view_y(gd, py);

	sy = grid_view_y(gd, gd->sy);

	grid_move_lines(gd, py, py + ny, sy - py - ny, bg);
	grid_clear(gd, 0, sy - ny, gd->sx, ny, bg);
}

/* Delete lines inside scroll region. */
void
grid_view_delete_lines_region(struct grid *gd, u_int rlower, u_int py,
    u_int ny, u_int rleft, u_int rright, u_int bg)
{
	u_int	ny2, nx;

	rlower = grid_view_y(gd, rlower);
	rleft = grid_view_x(gd, rleft);
	rright = grid_view_x(gd, rright);

	py = grid_view_y(gd, py);

	ny2 = rlower + 1 - py - ny;
	nx = rright - rleft + 1;
	grid_move_rect(gd, rleft, py, rleft, py + ny, nx, ny2, bg);
	grid_clear(gd, rleft, py + ny2, nx, ny - ny2, bg);
}

/* Insert characters. */
void
grid_view_insert_cells(struct grid *gd, u_int rright, u_int px, u_int py,
    u_int nx, u_int bg)
{
	rright = grid_view_x(gd, rright);

	px = grid_view_x(gd, px);
	py = grid_view_y(gd, py);

	if (px == rright)
		grid_clear(gd, px, py, 1, 1, bg);
	else
		grid_move_cells(gd, px + nx, px, py, rright + 1 - px - nx, bg);
}

/* Delete characters. */
void
grid_view_delete_cells(struct grid *gd, u_int rright, u_int px, u_int py,
    u_int nx, u_int bg)
{
	rright = grid_view_x(gd, rright);

	px = grid_view_x(gd, px);
	py = grid_view_y(gd, py);

	grid_move_cells(gd, px, px + nx, py, rright + 1 - px - nx, bg);
	grid_clear(gd, rright + 1 - nx, py, nx, 1, bg);
}

/* Insert columns in region. */
void
grid_view_insert_columns(struct grid *gd, u_int rright, u_int px, u_int nx,
    u_int rupper, u_int rlower, u_int bg)
{
	u_int	nx2, ny;

	rright = grid_view_x(gd, rright);
	rupper = grid_view_y(gd, rupper);
	rlower = grid_view_y(gd, rlower);

	px = grid_view_x(gd, px);

	nx2 = rright + 1 - px - nx;
	ny = rlower - rupper + 1;
	grid_move_rect(gd, rright + 1 - nx2, rupper, px, rupper, nx2, ny, bg);
	grid_clear(gd, px + nx2, rupper, nx - nx2, ny, bg);
}

/* Delete columns inside scroll region. */
void
grid_view_delete_columns(struct grid *gd, u_int rright, u_int px, u_int nx,
    u_int rupper, u_int rlower, u_int bg)
{
	u_int	nx2, ny;

	rright = grid_view_x(gd, rright);
	rupper = grid_view_y(gd, rupper);
	rlower = grid_view_y(gd, rlower);

	px = grid_view_x(gd, px);

	nx2 = rright + 1 - px - nx;
	ny = rlower - rupper + 1;
	grid_move_rect(gd, px, rupper, px + nx, rupper, nx2, ny, bg);
	grid_clear(gd, px + nx2, rupper, nx - nx2, ny, bg);
}

/* Convert cells into a string. */
char *
grid_view_string_cells(struct grid *gd, u_int px, u_int py, u_int nx)
{
	px = grid_view_x(gd, px);
	py = grid_view_y(gd, py);

	return (grid_string_cells(gd, px, py, nx, NULL, 0, NULL));
}
