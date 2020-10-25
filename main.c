#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <linux/media.h>
#include <linux/v4l2-subdev.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <linux/kdev_t.h>
#include <sys/sysmacros.h>
#include <asm/errno.h>
#include <wordexp.h>
#include <gtk/gtk.h>
#include <tiffio.h>
#include <locale.h>
#include "config.h"
#include "ini.h"
#include "quickdebayer.h"
#include "camera.h"
#include "device.h"
#include "pipeline.h"

enum user_control {
	USER_CONTROL_ISO,
	USER_CONTROL_SHUTTER
};

#define TIFFTAG_FORWARDMATRIX1 50964

struct buffer {
	void *start;
	size_t length;
};

struct camerainfo {
	char dev_name[260];
	uint32_t pad_id;
	char dev[260];
	MPCamera *camera;
	MPCameraMode camera_mode;
	int fd;
	int rotate;

	float colormatrix[9];
	float forwardmatrix[9];
	int blacklevel;
	int whitelevel;

	float focallength;
	float cropfactor;
	double fnumber;
	int iso_min;
	int iso_max;

	int gain_ctrl;
	int gain_max;

	int has_af_c;
	int has_af_s;
};

static float colormatrix_srgb[] = {
	3.2409, -1.5373, -0.4986,
	-0.9692, 1.8759, 0.0415,
	0.0556, -0.2039, 1.0569
};

struct camerainfo rear_cam;
struct camerainfo front_cam;
struct camerainfo *current_cam;

// Camera interface
static char *media_drv_name;
static uint32_t interface_pad_id;
static char dev_name[260];
static int video_fd;
static char *exif_make;
static char *exif_model;

// State
static cairo_surface_t *surface = NULL;
static cairo_surface_t *status_surface = NULL;
static int preview_width = -1;
static int preview_height = -1;
static char last_path[260] = "";
static int auto_exposure = 1;
static int exposure = 1;
static int auto_gain = 1;
static int gain = 1;
static int burst_length = 10;
static char burst_dir[23];
static char processing_script[512];
static enum user_control current_control;
// Widgets
GtkWidget *preview;
GtkWidget *error_box;
GtkWidget *error_message;
GtkWidget *main_stack;
GtkWidget *thumb_last;
GtkWidget *control_box;
GtkWidget *control_name;
GtkAdjustment *control_slider;
GtkWidget *control_auto;

static int
xioctl(int fd, int request, void *arg)
{
	int r;
	do {
		r = ioctl(fd, request, arg);
	} while (r == -1 && errno == EINTR);
	return r;
}

static void
show_error(const char *s)
{
	gtk_label_set_text(GTK_LABEL(error_message), s);
	gtk_widget_show(error_box);
}

int
remap(int value, int input_min, int input_max, int output_min, int output_max)
{
	const long long factor = 1000000000;
	long long output_spread = output_max - output_min;
	long long input_spread = input_max - input_min;

	long long zero_value = value - input_min;
	zero_value *= factor;
	long long percentage = zero_value / input_spread;

	long long zero_output = percentage * output_spread / factor;

	long long result = output_min + zero_output;
	return (int)result;
}

static int
v4l2_ctrl_set(int fd, uint32_t id, int val)
{
	struct v4l2_control ctrl = {0};
	ctrl.id = id;
	ctrl.value = val;

	if (xioctl(fd, VIDIOC_S_CTRL, &ctrl) == -1) {
		g_printerr("Failed to set control %d to %d\n", id, val);
		return -1;
	}
	return 0;
}

static int
v4l2_ctrl_get(int fd, uint32_t id)
{
	struct v4l2_control ctrl = {0};
	ctrl.id = id;

	if (xioctl(fd, VIDIOC_G_CTRL, &ctrl) == -1) {
		g_printerr("Failed to get control %d\n", id);
		return -1;
	}
	return ctrl.value;
}

static int
v4l2_ctrl_get_max(int fd, uint32_t id)
{
	struct v4l2_queryctrl queryctrl;
	int ret;

	memset(&queryctrl, 0, sizeof(queryctrl));

	queryctrl.id = id;
	ret = xioctl(fd, VIDIOC_QUERYCTRL, &queryctrl);
	if (ret)
		return 0;

	if (queryctrl.flags & V4L2_CTRL_FLAG_DISABLED) {
		return 0;
	}

	return queryctrl.maximum;
}

static int
v4l2_has_control(int fd, int control_id)
{
	struct v4l2_queryctrl queryctrl;
	int ret;

	memset(&queryctrl, 0, sizeof(queryctrl));

	queryctrl.id = control_id;
	ret = xioctl(fd, VIDIOC_QUERYCTRL, &queryctrl);
	if (ret)
		return 0;

	if (queryctrl.flags & V4L2_CTRL_FLAG_DISABLED) {
		return 0;
	}

	return 1;
}

static void
draw_controls()
{
	cairo_t *cr;
	char iso[6];
	int temp;
	char shutterangle[6];

	if (auto_exposure) {
		sprintf(shutterangle, "auto");
	} else {
		temp = (int)((float)exposure / (float)current_cam->camera_mode.height * 360);
		sprintf(shutterangle, "%d\u00b0", temp);
	}

	if (auto_gain) {
		sprintf(iso, "auto");
	} else {
		temp = remap(gain - 1, 0, current_cam->gain_max, current_cam->iso_min, current_cam->iso_max);
		sprintf(iso, "%d", temp);
	}

	if (status_surface)
		cairo_surface_destroy(status_surface);

	// Make a service to show status of controls, 32px high
	if (gtk_widget_get_window(preview) == NULL) {
		return;
	}
	status_surface = gdk_window_create_similar_surface(gtk_widget_get_window(preview),
		CAIRO_CONTENT_COLOR_ALPHA,
		preview_width, 32);

	cr = cairo_create(status_surface);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.0);
	cairo_paint(cr);

	// Draw the outlines for the headings
	cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
	cairo_set_font_size(cr, 9);
	cairo_set_source_rgba(cr, 0, 0, 0, 1);

	cairo_move_to(cr, 16, 16);
	cairo_text_path(cr, "ISO");
	cairo_stroke(cr);

	cairo_move_to(cr, 60, 16);
	cairo_text_path(cr, "Shutter");
	cairo_stroke(cr);

	// Draw the fill for the headings
	cairo_set_source_rgba(cr, 1, 1, 1, 1);
	cairo_move_to(cr, 16, 16);
	cairo_show_text(cr, "ISO");
	cairo_move_to(cr, 60, 16);
	cairo_show_text(cr, "Shutter");

	// Draw the outlines for the values
	cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 11);
	cairo_set_source_rgba(cr, 0, 0, 0, 1);

	cairo_move_to(cr, 16, 26);
	cairo_text_path(cr, iso);
	cairo_stroke(cr);

	cairo_move_to(cr, 60, 26);
	cairo_text_path(cr, shutterangle);
	cairo_stroke(cr);

	// Draw the fill for the values
	cairo_set_source_rgba(cr, 1, 1, 1, 1);
	cairo_move_to(cr, 16, 26);
	cairo_show_text(cr, iso);
	cairo_move_to(cr, 60, 26);
	cairo_show_text(cr, shutterangle);

	cairo_destroy(cr);
	
}

static void
register_custom_tiff_tags(TIFF *tif)
{
	static const TIFFFieldInfo custom_fields[] = {
		{TIFFTAG_FORWARDMATRIX1, -1, -1, TIFF_SRATIONAL, FIELD_CUSTOM, 1, 1, "ForwardMatrix1"},
	};
	
	// Add missing dng fields
	TIFFMergeFieldInfo(tif, custom_fields, sizeof(custom_fields) / sizeof(custom_fields[0]));
}

static gboolean
preview_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
	cairo_set_source_surface(cr, surface, 0, 0);
	cairo_paint(cr);
	return FALSE;
}


static gboolean
preview_configure(GtkWidget *widget, GdkEventConfigure *event)
{
	cairo_t *cr;

	if (surface)
		cairo_surface_destroy(surface);

	surface = gdk_window_create_similar_surface(gtk_widget_get_window(widget),
		CAIRO_CONTENT_COLOR,
		gtk_widget_get_allocated_width(widget),
		gtk_widget_get_allocated_height(widget));

	preview_width = gtk_widget_get_allocated_width(widget);
	preview_height = gtk_widget_get_allocated_height(widget);

	cr = cairo_create(surface);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);
	cairo_destroy(cr);

	draw_controls();

	return TRUE;
}

int
strtoint(const char *nptr, char **endptr, int base)
{
	long x = strtol(nptr, endptr, base);
	assert(x <= INT_MAX);
	return (int) x;
}

static int
config_ini_handler(void *user, const char *section, const char *name,
	const char *value)
{
	struct camerainfo *cc;
	if (strcmp(section, "rear") == 0 || strcmp(section, "front") == 0) {
		if (strcmp(section, "rear") == 0) {
			cc = &rear_cam;
		} else {
			cc = &front_cam;
		}
		if (strcmp(name, "width") == 0) {
			cc->camera_mode.width = strtoint(value, NULL, 10);
		} else if (strcmp(name, "height") == 0) {
			cc->camera_mode.height = strtoint(value, NULL, 10);
		} else if (strcmp(name, "rate") == 0) {
			cc->camera_mode.frame_interval.numerator = 1;
			cc->camera_mode.frame_interval.denominator = strtoint(value, NULL, 10);
		} else if (strcmp(name, "rotate") == 0) {
			cc->rotate = strtoint(value, NULL, 10);
		} else if (strcmp(name, "fmt") == 0) {
			cc->camera_mode.pixel_format = mp_pixel_format_from_str(value);
			if (cc->camera_mode.pixel_format == MP_PIXEL_FMT_UNSUPPORTED) {
				g_printerr("Unsupported pixelformat %s\n", value);
				exit(1);
			}
		} else if (strcmp(name, "driver") == 0) {
			strcpy(cc->dev_name, value);
		} else if (strcmp(name, "colormatrix") == 0) {
			sscanf(value, "%f,%f,%f,%f,%f,%f,%f,%f,%f",
					cc->colormatrix+0,
					cc->colormatrix+1,
					cc->colormatrix+2,
					cc->colormatrix+3,
					cc->colormatrix+4,
					cc->colormatrix+5,
					cc->colormatrix+6,
					cc->colormatrix+7,
					cc->colormatrix+8
					);
		} else if (strcmp(name, "forwardmatrix") == 0) {
			sscanf(value, "%f,%f,%f,%f,%f,%f,%f,%f,%f",
					cc->forwardmatrix+0,
					cc->forwardmatrix+1,
					cc->forwardmatrix+2,
					cc->forwardmatrix+3,
					cc->forwardmatrix+4,
					cc->forwardmatrix+5,
					cc->forwardmatrix+6,
					cc->forwardmatrix+7,
					cc->forwardmatrix+8
					);
		} else if (strcmp(name, "whitelevel") == 0) {
			cc->whitelevel = strtoint(value, NULL, 10);
		} else if (strcmp(name, "blacklevel") == 0) {
			cc->blacklevel = strtoint(value, NULL, 10);
		} else if (strcmp(name, "focallength") == 0) {
			cc->focallength = strtof(value, NULL);
		} else if (strcmp(name, "cropfactor") == 0) {
			cc->cropfactor = strtof(value, NULL);
		} else if (strcmp(name, "fnumber") == 0) {
			cc->fnumber = strtod(value, NULL);
		} else if (strcmp(name, "iso-min") == 0) {
			cc->iso_min = strtod(value, NULL);
		} else if (strcmp(name, "iso-max") == 0) {
			cc->iso_max = strtod(value, NULL);
		} else {
			g_printerr("Unknown key '%s' in [%s]\n", name, section);
			exit(1);
		}
	} else if (strcmp(section, "device") == 0) {
		if (strcmp(name, "csi") == 0) {
			media_drv_name = strdup(value);
		} else if (strcmp(name, "make") == 0) {
			exif_make = strdup(value);
		} else if (strcmp(name, "model") == 0) {
			exif_model = strdup(value);
		} else {
			g_printerr("Unknown key '%s' in [device]\n", name);
			exit(1);
		}
	} else {
		g_printerr("Unknown section '%s' in config file\n", section);
		exit(1);
	}
	return 1;
}

static MPDevice *device = NULL;
static MPPipeline *capture_pipeline = NULL;
static MPPipelineCapture *pipeline_capture = NULL;
static MPPipeline *process_pipeline = NULL;

struct update_preview_args {
	GdkPixbuf *pixbuf;
	bool update_thumbnail;
};

static bool update_preview(struct update_preview_args *args)
{
	if (!surface)
		return false;

	if (args->update_thumbnail)
	{
		GdkPixbuf *thumb = gdk_pixbuf_scale_simple(args->pixbuf, 24, 24, GDK_INTERP_BILINEAR);
		gtk_image_set_from_pixbuf(GTK_IMAGE(thumb_last), thumb);
	}

	// Draw preview image
	double scale = (double) preview_width / gdk_pixbuf_get_width(args->pixbuf);
	cairo_t *cr = cairo_create(surface);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);
	cairo_scale(cr, scale, scale);
	gdk_cairo_set_source_pixbuf(cr, args->pixbuf, 0, 0);
	cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_NONE);
	cairo_paint(cr);

	// Draw controls over preview
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_set_source_surface(cr, status_surface, 0, 0);
	cairo_paint(cr);

	cairo_destroy(cr);

	// Queue gtk3 repaint of the preview area
	gtk_widget_queue_draw_area(preview, 0, 0, preview_width, preview_height);

	g_object_unref(args->pixbuf);

	return false;
}

static void process_image_for_preview(const MPImage *image, bool update_thumbnail)
{
	int skip = 0;
	if (current_cam->rotate == 0 || current_cam->rotate == 180) {
		skip = round((image->width / 2) / (float)preview_width);
	} else {
		skip = round((image->width / 2) / (float)preview_height);
	}
	skip += 1;

	if (skip < 0) {
		skip = 0;
	} else if (skip > 3) {
		skip = 3;
	}

	GdkPixbuf *pixbuf = gdk_pixbuf_new(
		GDK_COLORSPACE_RGB,
		FALSE,
		8,
		image->width / (skip*2),
		image->height / (skip*2));

	guchar *pixels = gdk_pixbuf_get_pixels(pixbuf);
	quick_debayer_bggr8(
		(const uint8_t *)image->data,
		pixels,
		image->width,
		image->height,
		skip,
		current_cam->blacklevel);

	GdkPixbuf *pixbufrot;
	if (current_cam->rotate == 0) {
		pixbufrot = pixbuf;
	} else if (current_cam->rotate == 90) {
		pixbufrot = gdk_pixbuf_rotate_simple(pixbuf, GDK_PIXBUF_ROTATE_COUNTERCLOCKWISE);
	} else if (current_cam->rotate == 180) {
		pixbufrot = gdk_pixbuf_rotate_simple(pixbuf, GDK_PIXBUF_ROTATE_UPSIDEDOWN);
	} else if (current_cam->rotate == 270) {
		pixbufrot = gdk_pixbuf_rotate_simple(pixbuf, GDK_PIXBUF_ROTATE_CLOCKWISE);
	}

	if (pixbuf != pixbufrot) {
		g_object_unref(pixbuf);
	}

	struct update_preview_args *args = malloc(sizeof(struct update_preview_args));
	args->pixbuf = pixbufrot;
	args->update_thumbnail = update_thumbnail;

	g_main_context_invoke_full(
		g_main_context_default(),
		G_PRIORITY_DEFAULT,
		(GSourceFunc)update_preview,
		args,
		free);
}

static void process_image_for_capture(const MPImage *image, uint8_t count)
{
	static const float neutral[] = {1.0, 1.0, 1.0};
	static const short cfapatterndim[] = {2, 2};
	static uint16_t isospeed[] = {0};

	time_t rawtime;
	time(&rawtime);
	struct tm tim = *(localtime(&rawtime));

	char datetime[20] = {0};
	strftime(datetime, 20, "%Y:%m:%d %H:%M:%S", &tim);

	char fname[255];
	sprintf(fname, "%s/%d.dng", burst_dir, count);

	// Get latest exposure and gain now the auto gain/exposure is disabled while capturing
	gain = v4l2_ctrl_get(current_cam->fd, current_cam->gain_ctrl);
	exposure = v4l2_ctrl_get(current_cam->fd, V4L2_CID_EXPOSURE);

	TIFF *tif = TIFFOpen(fname, "w");
	if(!tif) {
		printf("Could not open tiff\n");
	}

	// Define TIFF thumbnail
	TIFFSetField(tif, TIFFTAG_SUBFILETYPE, 1);
	TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, image->width >> 4);
	TIFFSetField(tif, TIFFTAG_IMAGELENGTH, image->height >> 4);
	TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
	TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
	TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
	TIFFSetField(tif, TIFFTAG_MAKE, exif_make);
	TIFFSetField(tif, TIFFTAG_MODEL, exif_model);
	TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
	TIFFSetField(tif, TIFFTAG_DATETIME, datetime);
	TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 3);
	TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
	TIFFSetField(tif, TIFFTAG_SOFTWARE, "Megapixels");
	long sub_offset = 0;
	TIFFSetField(tif, TIFFTAG_SUBIFD, 1, &sub_offset);
	TIFFSetField(tif, TIFFTAG_DNGVERSION, "\001\001\0\0");
	TIFFSetField(tif, TIFFTAG_DNGBACKWARDVERSION, "\001\0\0\0");
	char uniquecameramodel[255];
	sprintf(uniquecameramodel, "%s %s", exif_make, exif_model);
	TIFFSetField(tif, TIFFTAG_UNIQUECAMERAMODEL, uniquecameramodel);
	if(current_cam->colormatrix[0]) {
		TIFFSetField(tif, TIFFTAG_COLORMATRIX1, 9, current_cam->colormatrix);
	} else {
		TIFFSetField(tif, TIFFTAG_COLORMATRIX1, 9, colormatrix_srgb);
	}
	if(current_cam->forwardmatrix[0]) {
		TIFFSetField(tif, TIFFTAG_FORWARDMATRIX1, 9, current_cam->forwardmatrix);
	}
	TIFFSetField(tif, TIFFTAG_ASSHOTNEUTRAL, 3, neutral);
	TIFFSetField(tif, TIFFTAG_CALIBRATIONILLUMINANT1, 21);
	// Write black thumbnail, only windows uses this
	{
		unsigned char *buf = (unsigned char *)calloc(1, (int)image->width >> 4);
		for (int row = 0; row < image->height>>4; row++) {
			TIFFWriteScanline(tif, buf, row, 0);
		}
		free(buf);
	}
	TIFFWriteDirectory(tif);

	// Define main photo
	TIFFSetField(tif, TIFFTAG_SUBFILETYPE, 0);
	TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, image->width);
	TIFFSetField(tif, TIFFTAG_IMAGELENGTH, image->height);
	TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
	TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_CFA);
	TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
	TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
	TIFFSetField(tif, TIFFTAG_CFAREPEATPATTERNDIM, cfapatterndim);
	TIFFSetField(tif, TIFFTAG_CFAPATTERN, "\002\001\001\000"); // BGGR
	if(current_cam->whitelevel) {
		TIFFSetField(tif, TIFFTAG_WHITELEVEL, 1, &current_cam->whitelevel);
	}
	if(current_cam->blacklevel) {
		TIFFSetField(tif, TIFFTAG_BLACKLEVEL, 1, &current_cam->blacklevel);
	}
	TIFFCheckpointDirectory(tif);
	printf("Writing frame to %s\n", fname);

	unsigned char *pLine = (unsigned char*)malloc(image->width);
	for(int row = 0; row < image->height; row++){
		TIFFWriteScanline(tif, image->data + row * image->width, row, 0);
	}
	free(pLine);
	TIFFWriteDirectory(tif);

	// Add an EXIF block to the tiff
	TIFFCreateEXIFDirectory(tif);
	// 1 = manual, 2 = full auto, 3 = aperture priority, 4 = shutter priority
	if (auto_exposure) {
		TIFFSetField(tif, EXIFTAG_EXPOSUREPROGRAM, 2);
	} else {
		TIFFSetField(tif, EXIFTAG_EXPOSUREPROGRAM, 1);
	}

	float interval = current_cam->camera_mode.frame_interval.numerator / (float) current_cam->camera_mode.frame_interval.denominator;
	TIFFSetField(tif, EXIFTAG_EXPOSURETIME, interval / ((float)image->height / (float)exposure));
	isospeed[0] = (uint16_t)remap(gain - 1, 0, current_cam->gain_max, current_cam->iso_min, current_cam->iso_max);
	TIFFSetField(tif, EXIFTAG_ISOSPEEDRATINGS, 1, isospeed);
	TIFFSetField(tif, EXIFTAG_FLASH, 0);

	TIFFSetField(tif, EXIFTAG_DATETIMEORIGINAL, datetime);
	TIFFSetField(tif, EXIFTAG_DATETIMEDIGITIZED, datetime);
	if(current_cam->fnumber) {
		TIFFSetField(tif, EXIFTAG_FNUMBER, current_cam->fnumber);
	}
	if(current_cam->focallength) {
		TIFFSetField(tif, EXIFTAG_FOCALLENGTH, current_cam->focallength);
	}
	if(current_cam->focallength && current_cam->cropfactor) {
		TIFFSetField(tif, EXIFTAG_FOCALLENGTHIN35MMFILM, (short)(current_cam->focallength * current_cam->cropfactor));
	}
	uint64_t exif_offset = 0;
	TIFFWriteCustomDirectory(tif, &exif_offset);
	TIFFFreeDirectory(tif);

	// Update exif pointer
	TIFFSetDirectory(tif, 0);
	TIFFSetField(tif, TIFFTAG_EXIFIFD, exif_offset);
	TIFFRewriteDirectory(tif);

	TIFFClose(tif);
}

static void process_capture_burst()
{
	time_t rawtime;
	time(&rawtime);
	struct tm tim = *(localtime(&rawtime));

	char timestamp[30];
	strftime(timestamp, 30, "%Y%m%d%H%M%S", &tim);

	char fname_target[255];
	sprintf(fname_target, "%s/Pictures/IMG%s", getenv("HOME"), timestamp);

	// Start post-processing the captured burst
	char command[1024];
	g_print("Post process %s to %s.ext\n", burst_dir, fname_target);
	sprintf(command, "%s %s %s &", processing_script, burst_dir, fname_target);
	system(command);

	// Restore the auto exposure and gain if needed
	if (auto_exposure) {
		v4l2_ctrl_set(current_cam->fd, V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_AUTO);
	}
	if (auto_gain) {
		v4l2_ctrl_set(current_cam->fd, V4L2_CID_AUTOGAIN, 1);
	}
}

static volatile size_t pipeline_frames_received = 0;
static volatile size_t pipeline_frames_processed = 0;
static volatile uint8_t pipeline_capture_frames = 0;
static volatile uint8_t pipeline_capture_burst_size = 0;

static void pipeline_process_image(MPPipeline *pipeline, MPImage *image)
{
	bool update_thumbnail = false;

	if (pipeline_capture_frames > 0) {
		uint8_t count = pipeline_capture_burst_size - pipeline_capture_frames;
		--pipeline_capture_frames;

		process_image_for_capture(image, count);

		if (pipeline_capture_frames == 0) {
			process_capture_burst();
			update_thumbnail = true;
		}
	}

	process_image_for_preview(image, update_thumbnail);

	free(image->data);

	++pipeline_frames_processed;
}

static void pipeline_on_frame_received(MPImage image, void *data)
{
	// If we haven't processed the previous frame yet, drop this one
	if (pipeline_frames_processed != pipeline_frames_received
		&& pipeline_capture_frames == 0)
	{
		printf("Dropped frame\n");
		return;
	}

	// TODO: If the last image hasn't been processed yet we should drop this one

	// Copy from the camera buffer
	size_t size = mp_pixel_format_bytes_per_pixel(image.pixel_format) * image.width * image.height;
	uint8_t *buffer = malloc(size);
	memcpy(buffer, image.data, size);

	image.data = buffer;

	++pipeline_frames_received;

	mp_pipeline_invoke(process_pipeline, (MPPipelineCallback)pipeline_process_image, &image, sizeof(MPImage));
}

static void pipeline_swap_camera(MPPipeline *p, struct camerainfo **_info)
{
	struct camerainfo *info = *_info;

	if (pipeline_capture) {
		mp_pipeline_capture_end(pipeline_capture);
	}

	struct camerainfo *other = info == &front_cam ? &rear_cam : &front_cam;

	mp_device_setup_link(device, other->pad_id, interface_pad_id, false);
	mp_device_setup_link(device, info->pad_id, interface_pad_id, true);

	mp_camera_set_mode(info->camera, &info->camera_mode);
	pipeline_capture = mp_pipeline_capture_start(capture_pipeline, info->camera, pipeline_on_frame_received, NULL);

	current_cam = info;
}

static void pipeline_setup_camera(struct camerainfo *info)
{
	const struct media_v2_entity *entity = mp_device_find_entity(device, info->dev_name);
	if (!entity) {
		g_printerr("Count not find camera entity matching '%s'\n", info->dev_name);
		exit(EXIT_FAILURE);
	}

	const struct media_v2_pad *pad = mp_device_get_pad_from_entity(device, entity->id);

	info->pad_id = pad->id;

	const struct media_v2_interface *interface = mp_device_find_entity_interface(device, entity->id);

	if (!mp_find_device_path(interface->devnode, info->dev_name, 260)) {
		g_printerr("Count not find camera device path\n");
		exit(EXIT_FAILURE);
	}

	info->fd = open(info->dev_name, O_RDWR);
	if (info->fd == -1) {
		g_printerr("Could not open %s\n", info->dev_name);
		exit(EXIT_FAILURE);
	}

	info->camera = mp_camera_new(video_fd, info->fd);

	// Trigger continuous auto focus if the sensor supports it
	if (v4l2_has_control(info->fd, V4L2_CID_FOCUS_AUTO)) {
		info->has_af_c = 1;
		v4l2_ctrl_set(info->fd, V4L2_CID_FOCUS_AUTO, 1);
	}
	if (v4l2_has_control(info->fd, V4L2_CID_AUTO_FOCUS_START)) {
		info->has_af_s = 1;
	}

	if (v4l2_has_control(info->fd, V4L2_CID_GAIN)) {
		info->gain_ctrl = V4L2_CID_GAIN;
		info->gain_max = v4l2_ctrl_get_max(info->fd, V4L2_CID_GAIN);
	}

	if (v4l2_has_control(info->fd, V4L2_CID_ANALOGUE_GAIN)) {
		info->gain_ctrl = V4L2_CID_ANALOGUE_GAIN;
		info->gain_max = v4l2_ctrl_get_max(info->fd, V4L2_CID_ANALOGUE_GAIN);
	}
}

static void pipeline_setup(MPPipeline *pipeline, void *data)
{
	device = mp_device_find(media_drv_name);
	if (!device) {
		g_printerr("Could not find /dev/media* node matching '%s'\n", media_drv_name);
		exit(EXIT_FAILURE);
	}

	const struct media_v2_entity *entity = mp_device_find_entity(device, media_drv_name);
	if (!entity) {
		g_printerr("Count not find device video entity\n");
		exit(EXIT_FAILURE);
	}

	const struct media_v2_pad *pad = mp_device_get_pad_from_entity(device, entity->id);
	interface_pad_id = pad->id;

	const struct media_v2_interface *interface = mp_device_find_entity_interface(device, entity->id);
	if (!mp_find_device_path(interface->devnode, dev_name, 260)) {
		g_printerr("Count not find video path\n");
		exit(EXIT_FAILURE);
	}

	video_fd = open(dev_name, O_RDWR);
	if (video_fd == -1) {
		g_printerr("Could not open %s\n", dev_name);
		exit(EXIT_FAILURE);
	}

	pipeline_setup_camera(&front_cam);
	pipeline_setup_camera(&rear_cam);

	struct camerainfo *next = &rear_cam;
	pipeline_swap_camera(pipeline, &next);
}

void start_pipeline()
{
	capture_pipeline = mp_pipeline_new();
	process_pipeline = mp_pipeline_new();

	mp_pipeline_invoke(capture_pipeline, pipeline_setup, NULL, 0);

	auto_exposure = 1;
	auto_gain = 1;
	draw_controls();
}

void stop_pipeline()
{
	if (pipeline_capture) {
		mp_pipeline_capture_end(pipeline_capture);
	}

	mp_pipeline_free(capture_pipeline);
	mp_pipeline_free(process_pipeline);
}

static void pipeline_start_capture_impl(MPPipeline *pipeline, uint32_t *count)
{
	pipeline_capture_frames = *count;
	pipeline_capture_burst_size = *count;

	// Disable the autogain/exposure while taking the burst
	v4l2_ctrl_set(current_cam->fd, V4L2_CID_AUTOGAIN, 0);
	v4l2_ctrl_set(current_cam->fd, V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_MANUAL);
}

void pipeline_start_capture(uint32_t count)
{
	mp_pipeline_invoke(process_pipeline, (MPPipelineCallback)pipeline_start_capture_impl, &count, sizeof(uint32_t));
}

void
on_open_last_clicked(GtkWidget *widget, gpointer user_data)
{
	char uri[270];
	GError *error = NULL;

	if(strlen(last_path) == 0) {
		return;
	}
	sprintf(uri, "file://%s", last_path);
	if(!g_app_info_launch_default_for_uri(uri, NULL, &error)){
		g_printerr("Could not launch image viewer: %s\n", error->message);
	}
}

void
on_open_directory_clicked(GtkWidget *widget, gpointer user_data)
{
	char uri[270];
	GError *error = NULL;
	sprintf(uri, "file://%s/Pictures", getenv("HOME"));
	if(!g_app_info_launch_default_for_uri(uri, NULL, &error)){
		g_printerr("Could not launch image viewer: %s\n", error->message);
	}
}

void
on_shutter_clicked(GtkWidget *widget, gpointer user_data)
{
	char template[] = "/tmp/megapixels.XXXXXX";
	char *tempdir;
	tempdir = mkdtemp(template);

	if (tempdir == NULL) {
		g_printerr("Could not make capture directory %s\n", template);
		exit (EXIT_FAILURE);
	}

	strcpy(burst_dir, tempdir);

	pipeline_start_capture(burst_length);
}

void
on_preview_tap(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
	if (event->type != GDK_BUTTON_PRESS)
		return;

	// Handle taps on the controls
	if (event->y < 32) {
		if (gtk_widget_is_visible(control_box)) {
			gtk_widget_hide(control_box);
			return;
		} else {
			gtk_widget_show(control_box);
		}

		if (event->x < 60 ) {
			// ISO
			current_control = USER_CONTROL_ISO;
			gtk_label_set_text(GTK_LABEL(control_name), "ISO");
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control_auto), auto_gain);
			gtk_adjustment_set_lower(control_slider, 0.0);
			gtk_adjustment_set_upper(control_slider, (float)current_cam->gain_max);
			gtk_adjustment_set_value(control_slider, (double)gain);

		} else if (event->x > 60 && event->x < 120) {
			// Shutter angle
			current_control = USER_CONTROL_SHUTTER;
			gtk_label_set_text(GTK_LABEL(control_name), "Shutter");
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control_auto), auto_exposure);
			gtk_adjustment_set_lower(control_slider, 1.0);
			gtk_adjustment_set_upper(control_slider, 360.0);
			gtk_adjustment_set_value(control_slider, (double)exposure);
		}

		return;
	}

	// Tapped preview image itself, try focussing
	if (current_cam->has_af_s) {
		v4l2_ctrl_set(current_cam->fd, V4L2_CID_AUTO_FOCUS_STOP, 1);
		v4l2_ctrl_set(current_cam->fd, V4L2_CID_AUTO_FOCUS_START, 1);
	}
}

void
on_error_close_clicked(GtkWidget *widget, gpointer user_data)
{
	gtk_widget_hide(error_box);
}

void
on_camera_switch_clicked(GtkWidget *widget, gpointer user_data)
{
	struct camerainfo *next;
	if (current_cam == &rear_cam) {
		next = &front_cam;
	} else {
		next = &rear_cam;
	}

	mp_pipeline_invoke(capture_pipeline, (MPPipelineCallback)pipeline_swap_camera, &next, sizeof(struct camerainfo *));

	auto_exposure = 1;
	auto_gain = 1;
	draw_controls();
}

void
on_settings_btn_clicked(GtkWidget *widget, gpointer user_data)
{
	gtk_stack_set_visible_child_name(GTK_STACK(main_stack), "settings");
}

void
on_back_clicked(GtkWidget *widget, gpointer user_data)
{
	gtk_stack_set_visible_child_name(GTK_STACK(main_stack), "main");
}

void
on_control_auto_toggled(GtkToggleButton *widget, gpointer user_data)
{
	int fd = current_cam->fd;
	switch (current_control) {
		case USER_CONTROL_ISO:
			auto_gain = gtk_toggle_button_get_active(widget);
			if (auto_gain) {
				v4l2_ctrl_set(fd, V4L2_CID_AUTOGAIN, 1);
			} else {
				v4l2_ctrl_set(fd, V4L2_CID_AUTOGAIN, 0);
				gain = v4l2_ctrl_get(fd, V4L2_CID_GAIN);
				gtk_adjustment_set_value(control_slider, (double)gain);
			}
			break;
		case USER_CONTROL_SHUTTER:
			auto_exposure = gtk_toggle_button_get_active(widget);
			if (auto_exposure) {
				v4l2_ctrl_set(fd, V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_AUTO);
			} else {
				v4l2_ctrl_set(fd, V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_MANUAL);
				exposure = v4l2_ctrl_get(fd, V4L2_CID_EXPOSURE);
				gtk_adjustment_set_value(control_slider, (double)exposure);
			}
			break;
	}
	draw_controls();
}

void
on_control_slider_changed(GtkAdjustment *widget, gpointer user_data)
{
	double value = gtk_adjustment_get_value(widget);

	switch (current_control) {
		case USER_CONTROL_ISO:
			gain = (int)value;
			v4l2_ctrl_set(current_cam->fd, current_cam->gain_ctrl, gain);
			break;
		case USER_CONTROL_SHUTTER:
			// So far all sensors use exposure time in number of sensor rows
			exposure = (int)(value / 360.0 * current_cam->camera_mode.height);
			v4l2_ctrl_set(current_cam->fd, V4L2_CID_EXPOSURE, exposure);
			break;
	}
	draw_controls();
}

int
find_config(char *conffile)
{
	char buf[512];
	char *xdg_config_home;
	wordexp_t exp_result;
	FILE *fp;

	// Resolve XDG stuff
	if ((xdg_config_home = getenv("XDG_CONFIG_HOME")) == NULL) {
		xdg_config_home = "~/.config";
	}
	wordexp(xdg_config_home, &exp_result, 0);
	xdg_config_home = strdup(exp_result.we_wordv[0]);
	wordfree(&exp_result);

	if(access("/proc/device-tree/compatible", F_OK) != -1) {
		// Reads to compatible string of the current device tree, looks like:
		// pine64,pinephone-1.2\0allwinner,sun50i-a64\0
		fp = fopen("/proc/device-tree/compatible", "r");
		fgets(buf, 512, fp);
		fclose(fp);

		// Check config/%dt.ini in the current working directory
		sprintf(conffile, "config/%s.ini", buf);
		if(access(conffile, F_OK) != -1) {
			printf("Found config file at %s\n", conffile);
			return 0;
		}

		// Check for a config file in XDG_CONFIG_HOME
		sprintf(conffile, "%s/megapixels/config/%s.ini", xdg_config_home, buf);
		if(access(conffile, F_OK) != -1) {
			printf("Found config file at %s\n", conffile);
			return 0;
		}

		// Check user overridden /etc/megapixels/config/$dt.ini
		sprintf(conffile, "%s/megapixels/config/%s.ini", SYSCONFDIR, buf);
		if(access(conffile, F_OK) != -1) {
			printf("Found config file at %s\n", conffile);
			return 0;
		}
		// Check packaged /usr/share/megapixels/config/$dt.ini
		sprintf(conffile, "%s/megapixels/config/%s.ini", DATADIR, buf);
		if(access(conffile, F_OK) != -1) {
			printf("Found config file at %s\n", conffile);
			return 0;
		}
		printf("%s not found\n", conffile);
	} else {
		printf("Could not read device name from device tree\n");
	}

	// If all else fails, fall back to /etc/megapixels.ini
	sprintf(conffile, "/etc/megapixels.ini");
	if(access(conffile, F_OK) != -1) {
		printf("Found config file at %s\n", conffile);
		return 0;
	}
	return -1;
}

int
find_processor(char *script)
{
	char *xdg_config_home;
	char filename[] = "postprocess.sh";
	wordexp_t exp_result;

	// Resolve XDG stuff
	if ((xdg_config_home = getenv("XDG_CONFIG_HOME")) == NULL) {
		xdg_config_home = "~/.config";
	}
	wordexp(xdg_config_home, &exp_result, 0);
	xdg_config_home = strdup(exp_result.we_wordv[0]);
	wordfree(&exp_result);

	// Check postprocess.h in the current working directory
	sprintf(script, "%s", filename);
	if(access(script, F_OK) != -1) {
		sprintf(script, "./%s", filename);
		printf("Found postprocessor script at %s\n", script);
		return 0;
	}

	// Check for a script in XDG_CONFIG_HOME
	sprintf(script, "%s/megapixels/%s", xdg_config_home, filename);
	if(access(script, F_OK) != -1) {
		printf("Found postprocessor script at %s\n", script);
		return 0;
	}

	// Check user overridden /etc/megapixels/postprocessor.sh
	sprintf(script, "%s/megapixels/%s", SYSCONFDIR, filename);
	if(access(script, F_OK) != -1) {
		printf("Found postprocessor script at %s\n", script);
		return 0;
	}

	// Check packaged /usr/share/megapixels/postprocessor.sh
	sprintf(script, "%s/megapixels/%s", DATADIR, filename);
	if(access(script, F_OK) != -1) {
		printf("Found postprocessor script at %s\n", script);
		return 0;
	}

	return -1;
}

int
main(int argc, char *argv[])
{
	int ret;
	char conffile[512];

	ret = find_config(conffile);
	if (ret) {
		g_printerr("Could not find any config file\n");
		return ret;
	}
	ret = find_processor(processing_script);
	if (ret) {
		g_printerr("Could not find any post-process script\n");
		return ret;
	}

	setenv("LC_NUMERIC", "C", 1);

	TIFFSetTagExtender(register_custom_tiff_tags);

	gtk_init(&argc, &argv);
	g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, NULL);
	GtkBuilder *builder = gtk_builder_new_from_resource("/org/postmarketos/Megapixels/camera.glade");

	GtkWidget *window = GTK_WIDGET(gtk_builder_get_object(builder, "window"));
	GtkWidget *shutter = GTK_WIDGET(gtk_builder_get_object(builder, "shutter"));
	GtkWidget *switch_btn = GTK_WIDGET(gtk_builder_get_object(builder, "switch_camera"));
	GtkWidget *settings_btn = GTK_WIDGET(gtk_builder_get_object(builder, "settings"));
	GtkWidget *settings_back = GTK_WIDGET(gtk_builder_get_object(builder, "settings_back"));
	GtkWidget *error_close = GTK_WIDGET(gtk_builder_get_object(builder, "error_close"));
	GtkWidget *open_last = GTK_WIDGET(gtk_builder_get_object(builder, "open_last"));
	GtkWidget *open_directory = GTK_WIDGET(gtk_builder_get_object(builder, "open_directory"));
	preview = GTK_WIDGET(gtk_builder_get_object(builder, "preview"));
	error_box = GTK_WIDGET(gtk_builder_get_object(builder, "error_box"));
	error_message = GTK_WIDGET(gtk_builder_get_object(builder, "error_message"));
	main_stack = GTK_WIDGET(gtk_builder_get_object(builder, "main_stack"));
	thumb_last = GTK_WIDGET(gtk_builder_get_object(builder, "thumb_last"));
	control_box = GTK_WIDGET(gtk_builder_get_object(builder, "control_box"));
	control_name = GTK_WIDGET(gtk_builder_get_object(builder, "control_name"));
	control_slider = GTK_ADJUSTMENT(gtk_builder_get_object(builder, "control_adj"));
	control_auto = GTK_WIDGET(gtk_builder_get_object(builder, "control_auto"));
	g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
	g_signal_connect(shutter, "clicked", G_CALLBACK(on_shutter_clicked), NULL);
	g_signal_connect(error_close, "clicked", G_CALLBACK(on_error_close_clicked), NULL);
	g_signal_connect(switch_btn, "clicked", G_CALLBACK(on_camera_switch_clicked), NULL);
	g_signal_connect(settings_btn, "clicked", G_CALLBACK(on_settings_btn_clicked), NULL);
	g_signal_connect(settings_back, "clicked", G_CALLBACK(on_back_clicked), NULL);
	g_signal_connect(open_last, "clicked", G_CALLBACK(on_open_last_clicked), NULL);
	g_signal_connect(open_directory, "clicked", G_CALLBACK(on_open_directory_clicked), NULL);
	g_signal_connect(preview, "draw", G_CALLBACK(preview_draw), NULL);
	g_signal_connect(preview, "configure-event", G_CALLBACK(preview_configure), NULL);
	gtk_widget_set_events(preview, gtk_widget_get_events(preview) |
			GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK);
	g_signal_connect(preview, "button-press-event", G_CALLBACK(on_preview_tap), NULL);
	g_signal_connect(control_auto, "toggled", G_CALLBACK(on_control_auto_toggled), NULL);
	g_signal_connect(control_slider, "value-changed", G_CALLBACK(on_control_slider_changed), NULL);

	GtkCssProvider *provider = gtk_css_provider_new();
	if (access("camera.css", F_OK) != -1) {
		gtk_css_provider_load_from_path(provider, "camera.css", NULL);
	} else {
		gtk_css_provider_load_from_resource(provider, "/org/postmarketos/Megapixels/camera.css");
	}
	GtkStyleContext *context = gtk_widget_get_style_context(error_box);
	gtk_style_context_add_provider(context,
		GTK_STYLE_PROVIDER(provider),
		GTK_STYLE_PROVIDER_PRIORITY_USER);
	context = gtk_widget_get_style_context(control_box);
	gtk_style_context_add_provider(context,
		GTK_STYLE_PROVIDER(provider),
		GTK_STYLE_PROVIDER_PRIORITY_USER);

	int result = ini_parse(conffile, config_ini_handler, NULL);
	if (result == -1) {
		g_printerr("Config file not found\n");
		return 1;
	} else if (result == -2) {
		g_printerr("Could not allocate memory to parse config file\n");
		return 1;
	} else if (result != 0) {
		g_printerr("Could not parse config file\n");
		return 1;
	}
	start_pipeline();

	gtk_widget_show(window);
	gtk_main();

	stop_pipeline();

	return 0;
}
