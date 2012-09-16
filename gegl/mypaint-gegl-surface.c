#include <stdlib.h>
#include <assert.h>

#include <mypaint-tiled-surface.h>
#include "mypaint-gegl-surface.h"
#include <gegl-utils.h>

#define TILE_SIZE 64

typedef struct _MyPaintGeglTiledSurface {
    MyPaintTiledSurface parent;

    int atomic;
    //Rect dirty_bbox; TODO: change into a GeglRectangle

    GeglRectangle extent_rect; // TODO: remove, just use the extent of the buffer
    GeglBuffer *buffer;
    const Babl *format;
} MyPaintGeglTiledSurface;

void free_gegl_tiledsurf(MyPaintSurface *surface);

void begin_atomic_gegl(MyPaintSurface *surface)
{
    MyPaintGeglTiledSurface *self = (MyPaintGeglTiledSurface *)surface;

    mypaint_tiled_surface_begin_atomic((MyPaintTiledSurface *)self);

    if (self->atomic == 0) {
      //assert(self->dirty_bbox.w == 0);
    }
    self->atomic++;
}

void end_atomic_gegl(MyPaintSurface *surface)
{
    MyPaintGeglTiledSurface *self = (MyPaintGeglTiledSurface *)surface;

    mypaint_tiled_surface_end_atomic((MyPaintTiledSurface *)self);

    assert(self->atomic > 0);
    self->atomic--;

    if (self->atomic == 0) {
      //Rect bbox = self->dirty_bbox;
      //self->dirty_bbox.w = 0;
      //if (bbox.w > 0) {
         // TODO: Could notify of changes here instead of for each tile changed
      //}
    }
}

static void
tile_request_start(MyPaintTiledSurface *tiled_surface, MyPaintTiledSurfaceTileRequestData *request)
{
    MyPaintGeglTiledSurface *self = (MyPaintGeglTiledSurface *)tiled_surface;

    GeglRectangle tile_bbox;
    gegl_rectangle_set(&tile_bbox, request->tx * TILE_SIZE, request->ty * TILE_SIZE, TILE_SIZE, TILE_SIZE);

    int read_write_flags;

    if (request->readonly) {
        read_write_flags = GEGL_BUFFER_READ;
    } else {
        read_write_flags = GEGL_BUFFER_READWRITE;

        // Extend the bounding box
        gegl_rectangle_bounding_box(&self->extent_rect, &self->extent_rect, &tile_bbox);
        gboolean success = gegl_buffer_set_extent(self->buffer, &self->extent_rect);
        g_assert(success);
    }

    GeglBufferIterator *iterator = gegl_buffer_iterator_new(self->buffer, &tile_bbox, 0, self->format,
                                  read_write_flags, GEGL_ABYSS_NONE);

    // Read out
    gboolean completed = gegl_buffer_iterator_next(iterator);
    g_assert(completed);

    if (iterator->length != TILE_SIZE*TILE_SIZE) {
        g_critical("Unable to get tile aligned access to GeglBuffer");
        request->buffer = NULL;
    } else {
        request->buffer = (uint16_t *)(iterator->data[0]);
    }

    // So we can finish the iterator in tile_request_end()
    request->context = (void *)iterator;
}

static void
tile_request_end(MyPaintTiledSurface *tiled_surface, MyPaintTiledSurfaceTileRequestData *request)
{
    MyPaintGeglTiledSurface *self = (MyPaintGeglTiledSurface *)tiled_surface;
    GeglBufferIterator *iterator = (GeglBufferIterator *)request->context;

    if (iterator) {
        gegl_buffer_iterator_next(iterator);
        request->context = NULL;
    }
}

void area_changed_gegl(MyPaintTiledSurface *tiled_surface, int bb_x, int bb_y, int bb_w, int bb_h)
{
    MyPaintGeglTiledSurface *self = (MyPaintGeglTiledSurface *)tiled_surface;

    // TODO: use gegl_rectangle_bounding_box instead
    //ExpandRectToIncludePoint (&self->dirty_bbox, bb_x, bb_y);
    //ExpandRectToIncludePoint (&self->dirty_bbox, bb_x+bb_w-1, bb_y+bb_h-1);
}

void
save_png(MyPaintSurface *surface, const char *path,
         int x, int y, int width, int height)
{
    MyPaintGeglTiledSurface *self = (MyPaintGeglTiledSurface *)surface;
    GeglNode *graph, *save, *source;

    graph = gegl_node_new();
    source = gegl_node_new_child(graph, "operation", "gegl:buffer-source",
                                 "buffer", mypaint_gegl_tiled_surface_get_buffer(self), NULL);
    save = gegl_node_new_child(graph, "operation", "gegl:png-save", "path", path, NULL);
    gegl_node_link(source, save);

    gegl_node_process(save);
    g_object_unref(graph);
}

GeglBuffer *
mypaint_gegl_tiled_surface_get_buffer(MyPaintGeglTiledSurface *self)
{
    return self->buffer;
}

void
mypaint_gegl_tiled_surface_set_buffer(MyPaintGeglTiledSurface *self, GeglBuffer *buffer)
{
    if (buffer && self->buffer == buffer) {
        return;
    }

    if (self->buffer) {
        g_object_unref(self->buffer);
    }

    if (buffer) {
        g_return_if_fail(GEGL_IS_BUFFER(buffer));
        g_object_ref(buffer);
        self->buffer = buffer;
    } else {
        // Using GeglBuffer with aligned tiles for zero-copy access
        self->buffer = GEGL_BUFFER(g_object_new(GEGL_TYPE_BUFFER,
                          "x", self->extent_rect.x, "y", self->extent_rect.y,
                          "width", self->extent_rect.width, "height", self->extent_rect.height,
                          "format", self->format,
                          "tile-width", TILE_SIZE, "tile-height", TILE_SIZE,
                          NULL));
    }
    g_assert(GEGL_IS_BUFFER(self->buffer));
}

MyPaintGeglTiledSurface *
mypaint_gegl_tiled_surface_new()
{
    MyPaintGeglTiledSurface *self = (MyPaintGeglTiledSurface *)malloc(sizeof(MyPaintGeglTiledSurface));

    mypaint_tiled_surface_init(&self->parent);

    // MyPaintSurface vfuncs
    self->parent.parent.destroy = free_gegl_tiledsurf;
    self->parent.parent.save_png = save_png;
    self->parent.parent.begin_atomic = begin_atomic_gegl;
    self->parent.parent.end_atomic = end_atomic_gegl;

    // MyPaintTiledSurface vfuncs
    self->parent.tile_request_start = tile_request_start;
    self->parent.tile_request_end = tile_request_end;
    self->parent.area_changed = area_changed_gegl;

    self->atomic = 0;
    //self->dirty_bbox.w = 0;

    self->buffer = NULL;

    gegl_rectangle_set(&self->extent_rect, 0, 0, 0, 0);

    self->format = babl_format_new(babl_model ("R'aG'aB'aA"), babl_type ("u15"),
                             babl_component("R'a"), babl_component("G'a"), babl_component("B'a"), babl_component("A"),
                             NULL);
    g_assert(self->format);

    mypaint_gegl_tiled_surface_set_buffer(self, NULL);

    return self;
}

void free_gegl_tiledsurf(MyPaintSurface *surface)
{
    MyPaintGeglTiledSurface *self = (MyPaintGeglTiledSurface *)surface;

    mypaint_tiled_surface_destroy(&self->parent);
    g_object_unref(self->buffer);

    free(self);
}
