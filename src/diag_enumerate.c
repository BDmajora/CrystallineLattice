/* P0.1 — Resource enumeration (read-only, no master).
 * Walks connectors/CRTCs/planes via the atomic-capable node and prints a
 * topology dump. Compare against `drm_info` and `modetest -c -p`. */
#define _GNU_SOURCE
#include "platform.h"
#include "diagnostics.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static const char *conn_type_name(uint32_t t)
{
	switch (t) {
	case DRM_MODE_CONNECTOR_VGA:        return "VGA";
	case DRM_MODE_CONNECTOR_DVII:       return "DVI-I";
	case DRM_MODE_CONNECTOR_DVID:       return "DVI-D";
	case DRM_MODE_CONNECTOR_DVIA:       return "DVI-A";
	case DRM_MODE_CONNECTOR_Composite:  return "Composite";
	case DRM_MODE_CONNECTOR_SVIDEO:     return "S-Video";
	case DRM_MODE_CONNECTOR_LVDS:       return "LVDS";
	case DRM_MODE_CONNECTOR_Component:  return "Component";
	case DRM_MODE_CONNECTOR_9PinDIN:    return "DIN";
	case DRM_MODE_CONNECTOR_DisplayPort:return "DP";
	case DRM_MODE_CONNECTOR_HDMIA:      return "HDMI-A";
	case DRM_MODE_CONNECTOR_HDMIB:      return "HDMI-B";
	case DRM_MODE_CONNECTOR_TV:         return "TV";
	case DRM_MODE_CONNECTOR_eDP:        return "eDP";
	case DRM_MODE_CONNECTOR_VIRTUAL:    return "Virtual";
	case DRM_MODE_CONNECTOR_DSI:        return "DSI";
	case DRM_MODE_CONNECTOR_WRITEBACK:  return "Writeback";
	default:                            return "Unknown";
	}
}

static const char *plane_type_name(uint64_t t)
{
	switch (t) {
	case DRM_PLANE_TYPE_OVERLAY: return "OVERLAY";
	case DRM_PLANE_TYPE_PRIMARY: return "PRIMARY";
	case DRM_PLANE_TYPE_CURSOR:  return "CURSOR";
	default:                     return "?";
	}
}

static size_t edid_len(int fd, drmModeConnector *c)
{
	struct prop_map pm;
	if (prop_map_load(fd, c->connector_id, DRM_MODE_OBJECT_CONNECTOR, &pm) != 0)
		return 0;
	uint64_t blob_id;
	size_t len = 0;
	if (prop_value(&pm, "EDID", &blob_id) && blob_id) {
		drmModePropertyBlobRes *b = drmModeGetPropertyBlob(fd, (uint32_t)blob_id);
		if (b) {
			len = b->length;
			drmModeFreePropertyBlob(b);
		}
	}
	prop_map_free(&pm);
	return len;
}

static void dump_connectors(int fd, drmModeRes *res)
{
	printf("== Connectors (%d) ==\n", res->count_connectors);
	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
		if (!c)
			continue;
		const char *st = c->connection == DRM_MODE_CONNECTED ? "connected"
		        : c->connection == DRM_MODE_DISCONNECTED ? "disconnected"
		        : "unknown";
		printf("  connector %u  %s-%u  %s  encoder=%u  edid=%zub\n",
		       c->connector_id, conn_type_name(c->connector_type),
		       c->connector_type_id, st, c->encoder_id, edid_len(fd, c));
		for (int m = 0; m < c->count_modes; m++) {
			drmModeModeInfo *md = &c->modes[m];
			printf("      mode %2d: %s %dx%d@%d%s\n", m, md->name,
			       md->hdisplay, md->vdisplay, md->vrefresh,
			       (md->type & DRM_MODE_TYPE_PREFERRED) ? " (preferred)"
			                                            : "");
		}
		drmModeFreeConnector(c);
	}
}

static void dump_crtcs(int fd, drmModeRes *res)
{
	printf("== CRTCs (%d) ==\n", res->count_crtcs);
	for (int i = 0; i < res->count_crtcs; i++) {
		drmModeCrtc *cr = drmModeGetCrtc(fd, res->crtcs[i]);
		if (!cr)
			continue;
		printf("  crtc %u  idx=%d  active=%d  mode=%s\n", cr->crtc_id, i,
		       cr->mode_valid, cr->mode_valid ? cr->mode.name : "-");
		drmModeFreeCrtc(cr);
	}
}

static void dump_planes(int fd)
{
	drmModePlaneRes *pres = drmModeGetPlaneResources(fd);
	if (!pres) {
		LOG_ERR("drmModeGetPlaneResources: %s", strerror(errno));
		return;
	}
	printf("== Planes (%u) ==\n", pres->count_planes);
	for (uint32_t i = 0; i < pres->count_planes; i++) {
		drmModePlane *pl = drmModeGetPlane(fd, pres->planes[i]);
		if (!pl)
			continue;
		struct prop_map pm;
		uint64_t type = 0xff;
		if (prop_map_load(fd, pl->plane_id, DRM_MODE_OBJECT_PLANE, &pm) == 0)
			prop_value(&pm, "type", &type);
		printf("  plane %u  type=%s  possible_crtcs=0x%x  formats:",
		       pl->plane_id, plane_type_name(type), pl->possible_crtcs);
		for (uint32_t f = 0; f < pl->count_formats; f++) {
			uint32_t fmt = pl->formats[f];
			printf(" %c%c%c%c", fmt & 0xff, (fmt >> 8) & 0xff,
			       (fmt >> 16) & 0xff, (fmt >> 24) & 0xff);
		}
		printf("\n");
		prop_map_free(&pm);
		drmModeFreePlane(pl);
	}
	drmModeFreePlaneResources(pres);
}

int diag_enumerate(int argc, char **argv)
{
	int fd = drm_open(argc > 1 ? argv[1] : NULL);
	if (fd < 0)
		return 1;

	drmModeRes *res = drmModeGetResources(fd);
	if (!res) {
		LOG_ERR("drmModeGetResources: %s", strerror(errno));
		return 1;
	}
	printf("== Device ==\n  fb min/max %dx%d .. %dx%d\n",
	       res->min_width, res->min_height, res->max_width, res->max_height);
	dump_connectors(fd, res);
	dump_crtcs(fd, res);
	dump_planes(fd);
	drmModeFreeResources(res);
	return 0;
}
