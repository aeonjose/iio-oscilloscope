/**
 * Copyright (C) 2014 Analog Devices, Inc.
 *
 * Licensed under the GPL-2.
 *
 **/
#include <stdio.h>

#include <gtk/gtk.h>
#include <glib/gthread.h>
#include <gtkdatabox.h>
#include <gtkdatabox_grid.h>
#include <gtkdatabox_points.h>
#include <gtkdatabox_lines.h>
#include <math.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <malloc.h>
#include <values.h>
#include <sys/stat.h>
#include <string.h>

#include <iio.h>

#include "../osc.h"
#include "../iio_widget.h"
#include "../osc_plugin.h"
#include "../config.h"
#include "../eeprom.h"
#include "../ini/ini.h"

static const gdouble mhz_scale = 1000000.0;
static const gdouble khz_scale = 1000.0;
static char *dac_buf_filename = NULL;

static bool dds_activated, dds_disabled;
static struct iio_buffer *dds_buffer;

static struct iio_context *ctx;
static struct iio_device *dac, *adc;

static GtkWidget *rf_out;

static GtkWidget *dds_mode;
static GtkWidget *dac_buffer;
static GtkWidget *dds1_freq, *dds2_freq, *dds3_freq, *dds4_freq;
static GtkWidget *dds1_scale, *dds2_scale, *dds3_scale, *dds4_scale;
static GtkWidget *dds1_phase, *dds2_phase, *dds3_phase, *dds4_phase;
static GtkAdjustment *adj1_freq, *adj2_freq, *adj3_freq, *adj4_freq;
static GtkWidget *dds1_freq_l, *dds2_freq_l, *dds3_freq_l, *dds4_freq_l;
static GtkWidget *dds1_scale_l, *dds2_scale_l, *dds3_scale_l, *dds4_scale_l;
static GtkWidget *dds1_phase_l, *dds2_phase_l, *dds3_phase_l, *dds4_phase_l;
static GtkWidget *dds_I_l, *dds_I1_l, *dds_I2_l;
static GtkWidget *dds_Q_l, *dds_Q1_l, *dds_Q2_l;
static gulong dds1_freq_hid = 0, dds2_freq_hid = 0;
static gulong dds1_scale_hid = 0, dds2_scale_hid = 0;
static gulong dds1_phase_hid = 0, dds2_phase_hid = 0;

static GtkWidget *dac_interpolation;
static GtkWidget *dac_shift;

static GtkWidget *avg_I, *avg_Q;
static GtkWidget *span_I, *span_Q;
static GtkWidget *radius_IQ, *angle_IQ;

static struct iio_widget tx_widgets[100];
static struct iio_widget rx_widgets[100];
static struct iio_widget cal_widgets[100];
static unsigned int num_tx, num_rx, num_cal,
		num_adc_freq, num_dds2_freq, num_dds4_freq;

typedef struct _Dialogs Dialogs;
struct _Dialogs
{
	GtkWidget *calibrate;
	GtkWidget *filechooser;
};
static Dialogs dialogs;
static GtkWidget *cal_save, *cal_open, *cal_tx, *cal_rx;
static GtkWidget *I_dac_pha_adj, *I_dac_offs, *I_dac_fs_adj;
static GtkWidget *Q_dac_pha_adj, *Q_dac_offs, *Q_dac_fs_adj;
static GtkWidget *I_adc_offset_adj, *I_adc_gain_adj, *I_adc_phase_adj;
static GtkWidget *Q_adc_offset_adj, *Q_adc_gain_adj, *Q_adc_phase_adj;

static GtkWidget *ad9122_temp;

//static unsigned short temp_calibbias;

static void enable_dds(bool on_off);

static int oneover(const gchar *num)
{
	float close;

	close = powf(2.0, roundf(log2f(1.0 / atof(num))));
	return (int)close;

}

#if 0
static void dac_shift_update(void)
{
	tx_widgets[num_dac_shift].update(&tx_widgets[num_dac_shift]);
}
#endif

static void rf_out_update(void)
{
	char buf[1024], dds1_m[16], dds2_m[16];
	static GtkTextBuffer *tbuf = NULL;
	GtkTextIter iter;

	if (tbuf == NULL) {
		tbuf = gtk_text_buffer_new(NULL);
		gtk_text_view_set_buffer(GTK_TEXT_VIEW(rf_out), tbuf);
	}

	memset(buf, 0, 1024);

	sprintf(buf, "\n");
	gtk_text_buffer_set_text(tbuf, buf, -1);
	gtk_text_buffer_get_iter_at_line(tbuf, &iter, 1);
	sprintf(dds1_m, "1/%i", oneover(gtk_entry_get_text(GTK_ENTRY(dds1_scale))));
	sprintf(dds2_m, "1/%i", oneover(gtk_entry_get_text(GTK_ENTRY(dds2_scale))));
}

short convert(double scale, float val)
{
	return (unsigned short) (val * scale + 32767.0);
}

int analyse_wavefile(const char *file_name, char **buf, int *count)
{
	int ret, j, i = 0, size, rep, tx = 1;
	double max = 0.0, val[4], scale = 0.0;
	double i1, q1, i2, q2;
	char line[80];

	FILE *infile = fopen(file_name, "r");
	if (infile == NULL)
		return -3;

	if (fgets(line, 80, infile) != NULL) {
	if (strncmp(line, "TEXT", 4) == 0) {
		/* Unscaled samples need to be in the range +- 32767 */
		if (strncmp(line, "TEXTU", 5) == 0)
			scale = 1.0; /* scale up to 16-bit */
		ret = sscanf(line, "TEXT%*c REPEAT %d", &rep);
		if (ret != 1) {
			rep = 1;
		}
		size = 0;
		while (fgets(line, 80, infile)) {
			ret = sscanf(line, "%lf%*[, \t]%lf%*[, \t]%lf%*[, \t]%lf",
				     &val[0], &val[1], &val[2], &val[3]);

			if (!(ret == 4 || ret == 2)) {
				fclose(infile);
				return -2;
			}

			for (i = 0; i < ret; i++)
				if (fabs(val[i]) > max)
					max = fabs(val[i]);

			size += ((tx == 2) ? 8 : 4);


		}

	size *= rep;
	if (scale == 0.0)
		scale = 32767.0 / max;

	if (max > 32767.0)
		fprintf(stderr, "ERROR: DAC Waveform Samples > +/- 32767.0\n");

	*buf = malloc(size);
	if (*buf == NULL)
		return 0;

	unsigned long long *sample = *((unsigned long long **)buf);
	unsigned int *sample_32 = *((unsigned int **)buf);

	rewind(infile);

	if (fgets(line, 80, infile) != NULL) {
		if (strncmp(line, "TEXT", 4) == 0) {
			size = 0;
			i = 0;
			while (fgets(line, 80, infile)) {

				ret = sscanf(line, "%lf%*[, \t]%lf%*[, \t]%lf%*[, \t]%lf",
					     &i1, &q1, &i2, &q2);
				for (j = 0; j < rep; j++) {
					if (ret == 4 && tx == 2) {
						sample[i++] = ((unsigned long long)convert(scale, q2) << 48) +
							((unsigned long long)convert(scale, i2) << 32) +
							(convert(scale, q1) << 16) +
							(convert(scale, i1) << 0);

						size += 8;
					}
					if (ret == 2 && tx == 2) {
						sample[i++] = ((unsigned long long)convert(scale, q1) << 48) +
							((unsigned long long)convert(scale, i1) << 32) +
							(convert(scale, q1) << 16) +
							(convert(scale, i1) << 0);

						size += 8;
					}
					if (tx == 1) {
						sample_32[i++] = (convert(scale, q1) << 16) +
							(convert(scale, i1) << 0);

						size += 4;
					}
				}
			}
		}
	}

	fclose(infile);
	*count = size;

	}} else {
		fclose(infile);
		*buf = NULL;
		return -1;
	}

	return 0;
}

static void tx_update_values(void)
{
	iio_update_widgets(tx_widgets, num_tx);
	rf_out_update();
}

static void rx_update_values(void)
{
	iio_update_widgets(rx_widgets, num_rx);
	rx_update_labels();
}

static void cal_update_values(void)
{
	iio_update_widgets(cal_widgets, num_cal);
}

static void process_dac_buffer_file (const char *file_name)
{
	int ret, size = 0;
	struct stat st;
	char *buf = NULL, *tmp;
	FILE *infile;
	unsigned int i, nb_channels = iio_device_get_channels_count(dac);

	ret = analyse_wavefile(file_name, &buf, &size);
	if (ret == -3)
		return;

	if (ret == -1 || buf == NULL) {
		stat(file_name, &st);
		buf = malloc(st.st_size);
		if (buf == NULL)
			return;
		infile = fopen(file_name, "r");
		size = fread(buf, 1, st.st_size, infile);
		fclose(infile);
	}

	if (dds_buffer) {
		iio_buffer_destroy(dds_buffer);
		dds_buffer = NULL;
	}

	enable_dds(false);

	/* Enable all channels */
	for (i = 0; i < nb_channels; i++)
		iio_channel_enable(iio_device_get_channel(dac, i));

	dds_buffer = iio_device_create_buffer(dac, size / iio_device_get_sample_size(dac), true);
	if (!dds_buffer) {
		fprintf(stderr, "Unable to create buffer: %s\n", strerror(errno));
		free(buf);
		return;
	}

	memcpy(iio_buffer_start(dds_buffer), buf,
			iio_buffer_end(dds_buffer) - iio_buffer_start(dds_buffer));

	iio_buffer_push(dds_buffer);
	free(buf);

	tmp = strdup(file_name);
	if (dac_buf_filename)
		free(dac_buf_filename);
	dac_buf_filename = tmp;
	printf("Waveform loaded\n");
}

static void dac_buffer_config_file_set_cb (GtkFileChooser *chooser, gpointer data)
{
	char *file_name = gtk_file_chooser_get_filename(chooser);
	if (file_name)
		process_dac_buffer_file((const char *)file_name);
}

static int compare_gain(const char *a, const char *b) __attribute__((unused));
static int compare_gain(const char *a, const char *b)
{
	double val_a, val_b;
	sscanf(a, "%lf", &val_a);
	sscanf(b, "%lf", &val_b);

	if (val_a < val_b)
		return -1;
	else if(val_a > val_b)
		return 1;
	else
		return 0;
}

static void dds_locked_freq_cb(GtkToggleButton *btn, gpointer data)
{
	gdouble freq1 = gtk_spin_button_get_value(GTK_SPIN_BUTTON(dds1_freq));
	gdouble freq2 = gtk_spin_button_get_value(GTK_SPIN_BUTTON(dds2_freq));
	size_t mode = gtk_combo_box_get_active(GTK_COMBO_BOX(dds_mode));

	switch (mode) {
		case 1:
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(dds2_freq), freq1);
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(dds3_freq), freq1);
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(dds4_freq), freq1);
			break;
		case 2:
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(dds3_freq), freq1);
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(dds4_freq), freq2);
			break;
		case 0: /* Off */
		case 3: /* Independent I/Q Control */
		case 4: /* DAC output */
			break;
		default:
			printf("%s: unknown mode (%d)error\n", __func__, (int)mode);
			break;
	}
}


static void dds_locked_phase_cb(GtkToggleButton *btn, gpointer data)
{

	gdouble phase1 = gtk_spin_button_get_value(GTK_SPIN_BUTTON(dds1_phase));
	gdouble phase2 = gtk_spin_button_get_value(GTK_SPIN_BUTTON(dds2_phase));
	size_t mode = gtk_combo_box_get_active(GTK_COMBO_BOX(dds_mode));

	switch (mode) {
		case 1:
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(dds2_phase), phase1);
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(dds3_phase), phase1 + 90.0);
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(dds4_phase), phase1 + 90.0);
			break;
		case 2:
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(dds3_phase), phase1 + 90.0);
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(dds4_phase), phase2 + 90.0);
			break;
		case 0: /* Off */
		case 3: /* Independent I/Q Control */
		case 4: /* DAC output */
			break;
		default:
			printf("%s: unknown mode (%d)error\n", __func__, (int)mode);
			break;
	}
}
static void dds_locked_scale_cb(GtkSpinButton *btn, gpointer data)
{
	gdouble scale1 = gtk_spin_button_get_value(GTK_SPIN_BUTTON(dds1_scale));
	gdouble scale2 = gtk_spin_button_get_value(GTK_SPIN_BUTTON(dds2_scale));

	size_t mode = gtk_combo_box_get_active(GTK_COMBO_BOX(dds_mode));

	switch (mode) {
		case 1:
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(dds2_scale), scale1);
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(dds3_scale), scale1);
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(dds4_scale), scale1);
			break;
		case 2:
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(dds3_scale), scale1);
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(dds4_scale), scale2);
			break;
		case 0: /* Off */
		case 3: /* Independent I/Q Control */
		case 4: /* DAC output */
			break;
		default:
			printf("%s: unknown mode (%d)error\n", __func__, (int)mode);
			break;
	}
}

#if 0
static void display_temp(void *ptr)
{
	double temp;
	int tmp;
	char buf[25];

	while (!kill_thread) {
		if (set_dev_paths("axi-ad9144-hpc") < 0) {
			kill_thread = 1;
			break;
		}

		if (read_devattr_double("in_temp0_input", &temp) < 0) {
			/* Just assume it's 25C, units are in milli-degrees C */
			temp = 25 * 1000;
			write_devattr_double("in_temp0_input", temp);
			read_devattr_int("in_temp0_calibbias", &tmp);
			/* This will eventually be stored in the EEPROM */
			temp_calibbias = tmp;
			printf("AD9122 temp cal value : %i\n", tmp);
		} else {

			sprintf(buf, "%2.1f", temp/1000);
			gdk_threads_enter();
			gtk_label_set_text(GTK_LABEL(ad9122_temp), buf);
			gdk_threads_leave();
		}
		usleep(500000);
	}
}
#endif

#define MATCH_SECT(s) (strcmp(section, s) == 0)
#define MATCH_NAME(n) (strcmp(name, n) == 0)
#define RX_F   "Rx_Frequency"
#define TX_F   "Tx_Frequency"
#define DDS1_F "DDS1_Frequency"
#define DDS1_S "DDS1_Scale"
#define DDS1_P "DDS1_Phase"
#define DDS2_F "DDS2_Frequency"
#define DDS2_S "DDS2_Scale"
#define DDS2_P "DDS2_Phase"
#define DDS3_F "DDS3_Frequency"
#define DDS3_S "DDS3_Scale"
#define DDS3_P "DDS3_Phase"
#define DDS4_F "DDS4_Frequency"
#define DDS4_S "DDS4_Scale"
#define DDS4_P "DDS4_Phase"

#define DAC_I_P "I_pha_adj"
#define DAC_I_O "I_dac_offs"
#define DAC_I_G "I_fs_adj"
#define DAC_Q_P "Q_pha_adj"
#define DAC_Q_O "Q_dac_offs"
#define DAC_Q_G "Q_fs_adj"

#define ADC_I_O "I_adc_offset_adj"
#define ADC_I_G "I_adc_gain_adj"
#define ADC_I_P "I_adc_phase_adj"
#define ADC_Q_O "Q_adc_offset_adj"
#define ADC_Q_G "Q_adc_gain_adj"
#define ADC_Q_P "Q_adc_phase_adj"

#if 0
static void combo_box_set_active_text(GtkWidget *combobox, const char* text)
{
	GtkTreeModel *tree = gtk_combo_box_get_model(GTK_COMBO_BOX(combobox));
	gboolean valid;
	GtkTreeIter iter;
	gint i = 0;

	valid = gtk_tree_model_get_iter_first (tree, &iter);
	while (valid) {
		gchar *str_data;

		gtk_tree_model_get(tree, &iter, 0, &str_data, -1);
		if (!strcmp(str_data, text)) {
			gtk_combo_box_set_active(GTK_COMBO_BOX(combobox), i);
			break;
		}

		i++;
		g_free (str_data);
		valid = gtk_tree_model_iter_next (tree, &iter);
	}
}
#endif

static void enable_dds(bool on_off)
{
	int ret;

	if (on_off == dds_activated && !dds_disabled)
		return;
	dds_activated = on_off;

	if (dds_buffer) {
		iio_buffer_destroy(dds_buffer);
		dds_buffer = NULL;
	}

	ret = iio_channel_attr_write_bool(iio_device_find_channel(dac, "altvoltage0", true), "raw", on_off);
	if (ret < 0) {
		fprintf(stderr, "Failed to toggle DDS: %d\n", ret);
		return;
	}
}

static void manage_dds_mode()
{
	gint active;

	rf_out_update();
	active = gtk_combo_box_get_active(GTK_COMBO_BOX(dds_mode));
	switch (active) {
	case 0:
		/* Disabled */
		enable_dds(false);
		gtk_widget_hide(dds1_freq);
		gtk_widget_hide(dds2_freq);
		gtk_widget_hide(dds3_freq);
		gtk_widget_hide(dds4_freq);
		gtk_widget_hide(dds1_scale);
		gtk_widget_hide(dds2_scale);
		gtk_widget_hide(dds3_scale);
		gtk_widget_hide(dds4_scale);
		gtk_widget_hide(dds1_phase);
		gtk_widget_hide(dds2_phase);
		gtk_widget_hide(dds3_phase);
		gtk_widget_hide(dds4_phase);
		gtk_widget_hide(dds1_freq_l);
		gtk_widget_hide(dds2_freq_l);
		gtk_widget_hide(dds3_freq_l);
		gtk_widget_hide(dds4_freq_l);
		gtk_widget_hide(dds1_scale_l);
		gtk_widget_hide(dds2_scale_l);
		gtk_widget_hide(dds3_scale_l);
		gtk_widget_hide(dds4_scale_l);
		gtk_widget_hide(dds1_phase_l);
		gtk_widget_hide(dds2_phase_l);
		gtk_widget_hide(dds3_phase_l);
		gtk_widget_hide(dds4_phase_l);
		gtk_widget_hide(dds_I_l);
		gtk_widget_hide(dds_I1_l);
		gtk_widget_hide(dds_I2_l);
		gtk_widget_hide(dds_Q_l);
		gtk_widget_hide(dds_Q1_l);
		gtk_widget_hide(dds_Q2_l);
		gtk_widget_hide(dac_buffer);
		if (!dds_activated && dds_buffer) {
			iio_buffer_destroy(dds_buffer);
			dds_buffer = NULL;
		}
		dds_disabled = true;
		break;
	case 1:
		/* One tone */
		enable_dds(true);
		gtk_widget_show(dds1_freq);
		gtk_widget_hide(dds2_freq);
		gtk_widget_hide(dds3_freq);
		gtk_widget_hide(dds4_freq);
		gtk_widget_show(dds1_scale);
		gtk_widget_hide(dds2_scale);
		gtk_widget_hide(dds3_scale);
		gtk_widget_hide(dds4_scale);
		gtk_widget_hide(dds1_phase);
		gtk_widget_hide(dds2_phase);
		gtk_widget_hide(dds3_phase);
		gtk_widget_hide(dds4_phase);
		gtk_widget_show(dds1_freq_l);
		gtk_widget_hide(dds2_freq_l);
		gtk_widget_hide(dds3_freq_l);
		gtk_widget_hide(dds4_freq_l);
		gtk_widget_show(dds1_scale_l);
		gtk_widget_hide(dds2_scale_l);
		gtk_widget_hide(dds3_scale_l);
		gtk_widget_hide(dds4_scale_l);
		gtk_widget_hide(dds1_phase_l);
		gtk_widget_hide(dds2_phase_l);
		gtk_widget_hide(dds3_phase_l);
		gtk_widget_hide(dds4_phase_l);
		gtk_widget_show(dds_I_l);
		gtk_label_set_markup(GTK_LABEL(dds_I_l), "<b>Single Tone</b>");
		gtk_widget_show(dds_I1_l);
		gtk_widget_hide(dds_I2_l);
		gtk_widget_hide(dds_Q_l);
		gtk_widget_hide(dds_Q1_l);
		gtk_widget_hide(dds_Q2_l);
		gtk_widget_hide(dac_buffer);

#define IIO_SPIN_SIGNAL "value-changed"
#define IIO_COMBO_SIGNAL "changed"

		if (!dds1_scale_hid)
			dds1_scale_hid = g_signal_connect(dds1_scale , IIO_SPIN_SIGNAL,
					G_CALLBACK(dds_locked_scale_cb), NULL);

		if (!dds1_freq_hid)
			dds1_freq_hid = g_signal_connect(dds1_freq , IIO_SPIN_SIGNAL,
					G_CALLBACK(dds_locked_freq_cb), NULL);

		if (!dds1_phase_hid)
			dds1_freq_hid = g_signal_connect(dds1_freq , IIO_SPIN_SIGNAL,
					G_CALLBACK(dds_locked_phase_cb), NULL);

		if (dds2_scale_hid) {
			g_signal_handler_disconnect(dds2_scale, dds2_scale_hid);
			dds2_scale_hid = 0;
		}

		if (dds2_freq_hid) {
			g_signal_handler_disconnect(dds2_freq, dds2_freq_hid);
			dds2_freq_hid = 0;
		}

		if (dds1_phase_hid) {
			g_signal_handler_disconnect(dds1_phase, dds1_phase_hid);
			dds1_phase_hid = 0;
		}
		if (dds2_phase_hid) {
			g_signal_handler_disconnect(dds2_phase, dds2_phase_hid);
			dds2_phase_hid = 0;
		}

		gtk_spin_button_set_value(GTK_SPIN_BUTTON(dds1_phase), 0.0);
		gtk_spin_button_set_value(GTK_SPIN_BUTTON(dds2_phase), 0.0);
		gtk_spin_button_set_value(GTK_SPIN_BUTTON(dds3_phase), 90.0);
		gtk_spin_button_set_value(GTK_SPIN_BUTTON(dds4_phase), 90.0);

		dds_locked_phase_cb(NULL, NULL);
		dds_locked_scale_cb(NULL, NULL);
		dds_locked_freq_cb(NULL, NULL);

		break;
	case 2:
		/* Two tones */
		enable_dds(true);
		gtk_widget_show(dds1_freq);
		gtk_widget_show(dds2_freq);
		gtk_widget_hide(dds3_freq);
		gtk_widget_hide(dds4_freq);
		gtk_widget_show(dds1_scale);
		gtk_widget_show(dds2_scale);
		gtk_widget_hide(dds3_scale);
		gtk_widget_hide(dds4_scale);
		gtk_widget_show(dds1_phase);
		gtk_widget_show(dds2_phase);
		gtk_widget_hide(dds3_phase);
		gtk_widget_hide(dds4_phase);
		gtk_widget_show(dds1_freq_l);
		gtk_widget_show(dds2_freq_l);
		gtk_widget_hide(dds3_freq_l);
		gtk_widget_hide(dds4_freq_l);
		gtk_widget_show(dds1_scale_l);
		gtk_widget_show(dds2_scale_l);
		gtk_widget_hide(dds3_scale_l);
		gtk_widget_hide(dds4_scale_l);
		gtk_widget_show(dds1_phase_l);
		gtk_widget_show(dds2_phase_l);
		gtk_widget_hide(dds3_phase_l);
		gtk_widget_hide(dds4_phase_l);
		gtk_widget_show(dds_I_l);
		gtk_label_set_markup(GTK_LABEL(dds_I_l), "<b>Two Tones</b>");
		gtk_widget_show(dds_I1_l);
		gtk_widget_show(dds_I2_l);
		gtk_widget_hide(dds_Q_l);
		gtk_widget_hide(dds_Q1_l);
		gtk_widget_hide(dds_Q2_l);
		gtk_widget_hide(dac_buffer);

		if (!dds1_scale_hid)
			dds1_scale_hid = g_signal_connect(dds1_scale , IIO_SPIN_SIGNAL,
					G_CALLBACK(dds_locked_scale_cb), NULL);
		if (!dds2_scale_hid)
			dds2_scale_hid = g_signal_connect(dds2_scale , IIO_SPIN_SIGNAL,
					G_CALLBACK(dds_locked_scale_cb), NULL);

		if (!dds1_freq_hid)
			dds1_freq_hid = g_signal_connect(dds1_freq , IIO_SPIN_SIGNAL,
					G_CALLBACK(dds_locked_freq_cb), NULL);
		if (!dds2_freq_hid)
			dds2_freq_hid = g_signal_connect(dds2_freq , IIO_SPIN_SIGNAL,
					G_CALLBACK(dds_locked_freq_cb), NULL);

		if (!dds1_phase_hid)
			dds1_phase_hid = g_signal_connect(dds1_phase , IIO_SPIN_SIGNAL,
					G_CALLBACK(dds_locked_phase_cb), NULL);
		if (!dds2_phase_hid)
			dds2_phase_hid = g_signal_connect(dds2_phase , IIO_SPIN_SIGNAL,
					G_CALLBACK(dds_locked_phase_cb), NULL);

		dds_locked_phase_cb(NULL, NULL);
		dds_locked_scale_cb(NULL, NULL);
		dds_locked_freq_cb(NULL, NULL);

		break;
	case 3:
		/* Independant/Individual control */
		enable_dds(true);
		gtk_widget_show(dds1_freq);
		gtk_widget_show(dds2_freq);
		gtk_widget_show(dds3_freq);
		gtk_widget_show(dds4_freq);
		gtk_widget_show(dds1_scale);
		gtk_widget_show(dds2_scale);
		gtk_widget_show(dds3_scale);
		gtk_widget_show(dds4_scale);
		gtk_widget_show(dds1_phase);
		gtk_widget_show(dds2_phase);
		gtk_widget_show(dds3_phase);
		gtk_widget_show(dds4_phase);
		gtk_widget_show(dds1_freq_l);
		gtk_widget_show(dds2_freq_l);
		gtk_widget_show(dds3_freq_l);
		gtk_widget_show(dds4_freq_l);
		gtk_widget_show(dds1_scale_l);
		gtk_widget_show(dds2_scale_l);
		gtk_widget_show(dds3_scale_l);
		gtk_widget_show(dds4_scale_l);
		gtk_widget_show(dds1_phase_l);
		gtk_widget_show(dds2_phase_l);
		gtk_widget_show(dds3_phase_l);
		gtk_widget_show(dds4_phase_l);
		gtk_widget_show(dds_I_l);
		gtk_label_set_markup(GTK_LABEL(dds_I_l), "<b>Channel I</b>");
		gtk_widget_show(dds_I1_l);
		gtk_widget_show(dds_I2_l);
		gtk_widget_show(dds_Q_l);
		gtk_widget_show(dds_Q1_l);
		gtk_widget_show(dds_Q2_l);
		gtk_widget_hide(dac_buffer);

		if (dds1_scale_hid) {
			g_signal_handler_disconnect(dds1_scale, dds1_scale_hid);
			dds1_scale_hid = 0;
		}
		if (dds2_scale_hid) {
			g_signal_handler_disconnect(dds2_scale, dds2_scale_hid);
			dds2_scale_hid = 0;
		}

		if (dds1_freq_hid) {
			g_signal_handler_disconnect(dds1_freq, dds1_freq_hid);
			dds1_freq_hid = 0;
		}
		if (dds2_freq_hid) {
			g_signal_handler_disconnect(dds2_freq, dds2_freq_hid);
			dds2_freq_hid = 0;
		}

		if (dds1_phase_hid) {
			g_signal_handler_disconnect(dds1_phase, dds1_phase_hid);
			dds1_phase_hid = 0;
		}

		if (dds2_phase_hid) {
			g_signal_handler_disconnect(dds2_phase, dds2_phase_hid);
			dds2_phase_hid = 0;
		}

		dds_locked_phase_cb(NULL, NULL);
		dds_locked_scale_cb(NULL, NULL);
		dds_locked_freq_cb(NULL, NULL);

		break;
	case 4:
		/* Buffer */
		if ((dds_activated || dds_disabled) && dac_buf_filename) {
			dds_disabled = false;
			process_dac_buffer_file(dac_buf_filename);
		}
		gtk_widget_hide(dds1_freq);
		gtk_widget_hide(dds2_freq);
		gtk_widget_hide(dds3_freq);
		gtk_widget_hide(dds4_freq);
		gtk_widget_hide(dds1_scale);
		gtk_widget_hide(dds2_scale);
		gtk_widget_hide(dds3_scale);
		gtk_widget_hide(dds4_scale);
		gtk_widget_hide(dds1_phase);
		gtk_widget_hide(dds2_phase);
		gtk_widget_hide(dds3_phase);
		gtk_widget_hide(dds4_phase);
		gtk_widget_hide(dds1_freq_l);
		gtk_widget_hide(dds2_freq_l);
		gtk_widget_hide(dds3_freq_l);
		gtk_widget_hide(dds4_freq_l);
		gtk_widget_hide(dds1_scale_l);
		gtk_widget_hide(dds2_scale_l);
		gtk_widget_hide(dds3_scale_l);
		gtk_widget_hide(dds4_scale_l);
		gtk_widget_hide(dds1_phase_l);
		gtk_widget_hide(dds2_phase_l);
		gtk_widget_hide(dds3_phase_l);
		gtk_widget_hide(dds4_phase_l);
		gtk_widget_hide(dds_I_l);
		gtk_widget_hide(dds_I1_l);
		gtk_widget_hide(dds_I2_l);
		gtk_widget_hide(dds_Q_l);
		gtk_widget_hide(dds_Q1_l);
		gtk_widget_hide(dds_Q2_l);
		gtk_widget_show(dac_buffer);
		break;
	default:
		printf("glade file out of sync with C file - please contact developers\n");
		break;
	}

}

#if 0
static void dac_cal_spin_helper(GtkRange *range,
		struct iio_channel *chn, const char *attr)
{
	gdouble inc, val = gtk_spin_button_get_value(GTK_SPIN_BUTTON(range));
	gtk_spin_button_get_increments(GTK_SPIN_BUTTON(range), &inc, NULL);

	if (inc == 1.0)
		iio_channel_attr_write_longlong(chn, attr, (long long) val);
	else
		iio_channel_attr_write_double(chn, attr, val);
}

static void dac_cal_spin0(GtkRange *range, gpointer user_data)
{
	dac_cal_spin_helper(range,
			iio_device_find_channel(dac, "voltage0", true),
			(const char *) user_data);
}

static void dac_cal_spin1(GtkRange *range, gpointer user_data)
{
	dac_cal_spin_helper(range,
			iio_device_find_channel(dac, "voltage1", true),
			(const char *) user_data);
}

static void adc_cal_spin_helper(GtkRange *range,
		struct iio_channel *chn, const char *attr)
{
	gdouble val, inc;

	val = gtk_spin_button_get_value(GTK_SPIN_BUTTON(range));
	gtk_spin_button_get_increments(GTK_SPIN_BUTTON(range), &inc, NULL);

	if (inc == 1.0)
		iio_channel_attr_write_longlong(chn, attr, (long long) val);
	else
		iio_channel_attr_write_double(chn, attr, val);
}

static void adc_cal_spin0(GtkRange *range, gpointer user_data)
{
	adc_cal_spin_helper(range,
			iio_device_find_channel(adc, "voltage0", false),
			(const char *) user_data);
}

static void adc_cal_spin1(GtkRange *range, gpointer user_data)
{
	adc_cal_spin_helper(range,
			iio_device_find_channel(adc, "voltage1", false),
			(const char *) user_data);
}
#endif

static void save_widget_value(GtkWidget *widget, struct iio_widget *iio_w)
{
	iio_w->save(iio_w);
}

static void make_widget_update_signal_based(struct iio_widget *widgets,
	unsigned int num_widgets)
{
	char signal_name[25];
	unsigned int i;

	for (i = 0; i < num_widgets; i++) {
		if (GTK_IS_CHECK_BUTTON(widgets[i].widget))
			sprintf(signal_name, "%s", "toggled");
		else if (GTK_IS_TOGGLE_BUTTON(widgets[i].widget))
			sprintf(signal_name, "%s", "toggled");
		else if (GTK_IS_SPIN_BUTTON(widgets[i].widget))
			sprintf(signal_name, "%s", "value-changed");
		else if (GTK_IS_COMBO_BOX_TEXT(widgets[i].widget))
			sprintf(signal_name, "%s", "changed");
		else
			printf("unhandled widget type, attribute: %s\n", widgets[i].attr_name);

		if (GTK_IS_SPIN_BUTTON(widgets[i].widget) &&
			widgets[i].priv_progress != NULL) {
				iio_spin_button_progress_activate(&widgets[i]);
		} else {
			g_signal_connect(G_OBJECT(widgets[i].widget), signal_name, G_CALLBACK(save_widget_value), &widgets[i]);
		}
	}
}

static GtkWidget * daq2_init(GtkWidget *notebook)
{
	GtkBuilder *builder;
	GtkWidget *daq2_panel;
	struct iio_channel *ch0, *ch1, *ch2, *ch3;

	ctx = osc_create_context();
	if (!ctx)
		return NULL;

	dac = iio_context_find_device(ctx, "axi-ad9144-hpc");
	adc = iio_context_find_device(ctx, "axi-ad9680-hpc");

	builder = gtk_builder_new();

	if (!gtk_builder_add_from_file(builder, "daq2.glade", NULL))
		gtk_builder_add_from_file(builder, OSC_GLADE_FILE_PATH "daq2.glade", NULL);


	daq2_panel = GTK_WIDGET(gtk_builder_get_object(builder, "daq2_panel"));


	avg_I = GTK_WIDGET(gtk_builder_get_object(builder, "avg_I"));
	avg_Q = GTK_WIDGET(gtk_builder_get_object(builder, "avg_Q"));
	span_I = GTK_WIDGET(gtk_builder_get_object(builder, "span_I"));
	span_Q = GTK_WIDGET(gtk_builder_get_object(builder, "span_Q"));
	radius_IQ = GTK_WIDGET(gtk_builder_get_object(builder, "radius_IQ"));
	angle_IQ = GTK_WIDGET(gtk_builder_get_object(builder, "angle_IQ"));


	dds_mode = GTK_WIDGET(gtk_builder_get_object(builder, "dds_mode"));

	dac_buffer = GTK_WIDGET(gtk_builder_get_object(builder, "dac_buffer"));

	dds_I_l  = GTK_WIDGET(gtk_builder_get_object(builder, "dds_tone_I_l"));

	dds_I1_l = GTK_WIDGET(gtk_builder_get_object(builder, "dds_tone_I1_l"));

	dds1_freq    = GTK_WIDGET(gtk_builder_get_object(builder, "dds_tone_I1_freq"));
	adj1_freq    = gtk_spin_button_get_adjustment(GTK_SPIN_BUTTON(dds1_freq));
	dds1_freq_l  = GTK_WIDGET(gtk_builder_get_object(builder, "dds_tone_I1_freq_l"));

	dds1_scale   = GTK_WIDGET(gtk_builder_get_object(builder, "dds_tone_I1_scale"));
	dds1_scale_l = GTK_WIDGET(gtk_builder_get_object(builder, "dds_tone_I1_scale_l"));

	dds1_phase   = GTK_WIDGET(gtk_builder_get_object(builder, "dds_tone_I1_phase"));
	dds1_phase_l = GTK_WIDGET(gtk_builder_get_object(builder, "dds_tone_I1_phase_l"));


	dds_I2_l = GTK_WIDGET(gtk_builder_get_object(builder, "dds_tone_I2_l"));

	dds2_freq = GTK_WIDGET(gtk_builder_get_object(builder, "dds_tone_I2_freq"));
	adj2_freq = gtk_spin_button_get_adjustment(GTK_SPIN_BUTTON(dds2_freq));
	dds2_freq_l  = GTK_WIDGET(gtk_builder_get_object(builder, "dds_tone_I2_freq_l"));

	dds2_scale = GTK_WIDGET(gtk_builder_get_object(builder, "dds_tone_I2_scale"));
	dds2_scale_l = GTK_WIDGET(gtk_builder_get_object(builder, "dds_tone_I2_scale_l"));

	dds2_phase   = GTK_WIDGET(gtk_builder_get_object(builder, "dds_tone_I2_phase"));
	dds2_phase_l = GTK_WIDGET(gtk_builder_get_object(builder, "dds_tone_I2_phase_l"));


	dds_Q_l  = GTK_WIDGET(gtk_builder_get_object(builder, "dds_tone_Q_l"));

	dds_Q1_l = GTK_WIDGET(gtk_builder_get_object(builder, "dds_tone_Q1_l"));

	dds3_freq = GTK_WIDGET(gtk_builder_get_object(builder, "dds_tone_Q1_freq"));
	adj3_freq = gtk_spin_button_get_adjustment(GTK_SPIN_BUTTON(dds3_freq));
	dds3_freq_l  = GTK_WIDGET(gtk_builder_get_object(builder, "dds_tone_Q1_freq_l"));

	dds3_scale = GTK_WIDGET(gtk_builder_get_object(builder, "dds_tone_Q1_scale"));
	dds3_scale_l = GTK_WIDGET(gtk_builder_get_object(builder, "dds_tone_Q1_scale_l"));

	dds3_phase =  GTK_WIDGET(gtk_builder_get_object(builder, "dds_tone_Q1_phase"));
	dds3_phase_l = GTK_WIDGET(gtk_builder_get_object(builder, "dds_tone_Q1_phase_l"));


	dds_Q2_l = GTK_WIDGET(gtk_builder_get_object(builder, "dds_tone_Q2_l"));

	dds4_freq = GTK_WIDGET(gtk_builder_get_object(builder, "dds_tone_Q2_freq"));
	adj4_freq = gtk_spin_button_get_adjustment(GTK_SPIN_BUTTON(dds4_freq));
	dds4_freq_l  = GTK_WIDGET(gtk_builder_get_object(builder, "dds_tone_Q2_freq_l"));

	dds4_scale = GTK_WIDGET(gtk_builder_get_object(builder, "dds_tone_Q2_scale"));
	dds4_scale_l = GTK_WIDGET(gtk_builder_get_object(builder, "dds_tone_Q2_scale_l"));

	dds4_phase =  GTK_WIDGET(gtk_builder_get_object(builder, "dds_tone_Q2_phase"));
	dds4_phase_l = GTK_WIDGET(gtk_builder_get_object(builder, "dds_tone_Q2_phase_l"));

	dialogs.calibrate =  GTK_WIDGET(gtk_builder_get_object(builder, "cal_dialog"));
	dialogs.filechooser = GTK_WIDGET(gtk_builder_get_object(builder, "filechooser"));

	cal_save = GTK_WIDGET(gtk_builder_get_object(builder, "Save"));
	cal_open = GTK_WIDGET(gtk_builder_get_object(builder, "Open"));
	cal_rx = GTK_WIDGET(gtk_builder_get_object(builder, "Cal_Rx"));
	cal_tx = GTK_WIDGET(gtk_builder_get_object(builder, "Cal_Tx"));

	I_dac_pha_adj = GTK_WIDGET(gtk_builder_get_object(builder, "dac_calibphase0"));
	I_dac_offs = GTK_WIDGET(gtk_builder_get_object(builder, "dac_calibbias0"));
	I_dac_fs_adj = GTK_WIDGET(gtk_builder_get_object(builder, "dac_calibscale0"));

	Q_dac_pha_adj = GTK_WIDGET(gtk_builder_get_object(builder, "dac_calibphase1"));
	Q_dac_offs = GTK_WIDGET(gtk_builder_get_object(builder, "dac_calibbias1"));
	Q_dac_fs_adj = GTK_WIDGET(gtk_builder_get_object(builder, "dac_calibscale1"));

	I_adc_offset_adj = GTK_WIDGET(gtk_builder_get_object(builder, "adc_calibbias0"));
	I_adc_gain_adj = GTK_WIDGET(gtk_builder_get_object(builder, "adc_calibscale0"));
	I_adc_phase_adj = GTK_WIDGET(gtk_builder_get_object(builder, "adc_calibphase0"));

	Q_adc_offset_adj = GTK_WIDGET(gtk_builder_get_object(builder, "adc_calibbias1"));
	Q_adc_gain_adj = GTK_WIDGET(gtk_builder_get_object(builder, "adc_calibscale1"));
	Q_adc_phase_adj = GTK_WIDGET(gtk_builder_get_object(builder, "adc_calibphase1"));

	ad9122_temp = GTK_WIDGET(gtk_builder_get_object(builder, "dac_temp"));

	rf_out =  GTK_WIDGET(gtk_builder_get_object(builder, "RF_out"));
	dac_interpolation = GTK_WIDGET(gtk_builder_get_object(builder, "dac_interpolation_clock"));
	dac_shift = GTK_WIDGET(gtk_builder_get_object(builder, "dac_fcenter_shift"));

	gtk_combo_box_set_active(GTK_COMBO_BOX(dds_mode), 1);
	manage_dds_mode();
	g_signal_connect( dds_mode, "changed", G_CALLBACK(manage_dds_mode), NULL);

	/* Bind the IIO device files to the GUI widgets */

	/* The next free frequency related widgets - keep in this order! */
	ch0 = iio_device_find_channel(dac, "altvoltage0", true);

	iio_spin_button_init_from_builder(&tx_widgets[num_tx++],
			dac, ch0, "sampling_frequency",
			builder, "dac_data_clock", &mhz_scale);
	iio_spin_button_add_progress(&tx_widgets[num_tx - 1]);

	/* DDS */
	ch1 = iio_device_find_channel(dac, "altvoltage1", true);
	ch2 = iio_device_find_channel(dac, "altvoltage2", true);
	ch3 = iio_device_find_channel(dac, "altvoltage3", true);
	iio_spin_button_init(&tx_widgets[num_tx++],
			dac, ch0, "frequency", dds3_freq, &mhz_scale);
	iio_spin_button_add_progress(&tx_widgets[num_tx - 1]);
	iio_spin_button_init(&tx_widgets[num_tx++],
			dac, ch2, "frequency", dds1_freq, &mhz_scale);
	iio_spin_button_add_progress(&tx_widgets[num_tx - 1]);
	num_dds4_freq = num_tx;
	iio_spin_button_init(&tx_widgets[num_tx++],
			dac, ch1, "frequency", dds4_freq, &mhz_scale);
	iio_spin_button_add_progress(&tx_widgets[num_tx - 1]);
	num_dds2_freq = num_tx;
	iio_spin_button_init(&tx_widgets[num_tx++],
			dac, ch3, "frequency", dds2_freq, &mhz_scale);
	iio_spin_button_add_progress(&tx_widgets[num_tx - 1]);

	iio_spin_button_init(&tx_widgets[num_tx++], dac, ch0, "scale", dds3_scale, NULL);
	iio_spin_button_add_progress(&tx_widgets[num_tx - 1]);
	iio_spin_button_init(&tx_widgets[num_tx++], dac, ch2, "scale", dds1_scale, NULL);
	iio_spin_button_add_progress(&tx_widgets[num_tx - 1]);
	iio_spin_button_init(&tx_widgets[num_tx++], dac, ch1, "scale", dds4_scale, NULL);
	iio_spin_button_add_progress(&tx_widgets[num_tx - 1]);
	iio_spin_button_init(&tx_widgets[num_tx++], dac, ch3, "scale", dds2_scale, NULL);
	iio_spin_button_add_progress(&tx_widgets[num_tx - 1]);

	iio_spin_button_init(&tx_widgets[num_tx++],
			dac, ch0, "phase", dds3_phase, &khz_scale);
	iio_spin_button_add_progress(&tx_widgets[num_tx - 1]);
	iio_spin_button_init(&tx_widgets[num_tx++],
			dac, ch2, "phase", dds1_phase, &khz_scale);
	iio_spin_button_add_progress(&tx_widgets[num_tx - 1]);
	iio_spin_button_init(&tx_widgets[num_tx++],
			dac, ch1, "phase", dds4_phase, &khz_scale);
	iio_spin_button_add_progress(&tx_widgets[num_tx - 1]);
	iio_spin_button_init(&tx_widgets[num_tx++],
			dac, ch3, "phase", dds2_phase, &khz_scale);
	iio_spin_button_add_progress(&tx_widgets[num_tx - 1]);

	/* Rx Widgets */

	ch0 = iio_device_find_channel(adc, "voltage0", false);

	num_adc_freq = num_rx;
	iio_spin_button_int_init_from_builder(&rx_widgets[num_rx++], adc, ch0,
			"sampling_frequency", builder, "adc_freq", &mhz_scale);
	iio_spin_button_add_progress(&rx_widgets[num_rx - 1]);

	g_builder_connect_signal(builder, "dac_buffer", "file-set",
		G_CALLBACK(dac_buffer_config_file_set_cb), NULL);

	g_signal_connect_after(dds1_scale, "change-value", G_CALLBACK(rf_out_update), NULL);
	g_signal_connect_after(dds2_scale, "change-value", G_CALLBACK(rf_out_update), NULL);

	make_widget_update_signal_based(rx_widgets, num_rx);
	make_widget_update_signal_based(tx_widgets, num_tx);

	iio_spin_button_set_on_complete_function(&rx_widgets[num_adc_freq], rx_update_labels);
	iio_spin_button_set_on_complete_function(&tx_widgets[num_dds2_freq], rf_out_update);
	iio_spin_button_set_on_complete_function(&tx_widgets[num_dds4_freq], rf_out_update);

	tx_update_values();
	rx_update_values();
	cal_update_values();

	gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER(dac_buffer), OSC_WAVEFORM_FILE_PATH);

	return daq2_panel;
}

#define SYNC_RELOAD "SYNC_RELOAD"

static char *handle_item(struct osc_plugin *plugin, const char *attrib,
			 const char *value)
{
	char *buf;

	if (MATCH_ATTRIB(SYNC_RELOAD)) {
		if (value) {
			tx_update_values();
			rx_update_values();
		} else {
			return "1";
		}
	} else if (MATCH_ATTRIB("dds_mode")) {
		if (value) {
			gtk_combo_box_set_active(GTK_COMBO_BOX(dds_mode), atoi(value));
		} else {
			buf = malloc (10);
			sprintf(buf, "%i", gtk_combo_box_get_active(GTK_COMBO_BOX(dds_mode)));
			return buf;
		}
	} else if (MATCH_ATTRIB("dac_buf_filename") &&
				gtk_combo_box_get_active(GTK_COMBO_BOX(dds_mode)) == 4) {
		if (value) {
			gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dac_buffer), value);
			process_dac_buffer_file(value);
		} else
			return dac_buf_filename;
	} else {
		if (value) {
			printf("Unhandled tokens in ini file,\n"
					"\tSection %s\n\tAtttribute : %s\n\tValue: %s\n",
					"DAQ2", attrib, value);
			return "FAIL";
		}
	}

	return NULL;
}

static void context_destroy(void)
{
	iio_context_destroy(ctx);
}

static const char *daq2_sr_attribs[] = {
	"axi-ad9680-hpc.in_voltage_sampling_frequency",
	"axi-ad9144-hpc.out_altvoltage_sampling_frequency",
	"dds_mode",
	"dac_buf_filename",
	"axi-ad9144-hpc.out_altvoltage0_1A_frequency",
	"axi-ad9144-hpc.out_altvoltage2_2A_frequency",
	"axi-ad9144-hpc.out_altvoltage1_1B_frequency",
	"axi-ad9144-hpc.out_altvoltage3_2B_frequency",
	"axi-ad9144-hpc.out_altvoltage0_1A_scale",
	"axi-ad9144-hpc.out_altvoltage2_2A_scale",
	"axi-ad9144-hpc.out_altvoltage1_1B_scale",
	"axi-ad9144-hpc.out_altvoltage3_2B_scale",
	"axi-ad9144-hpc.out_altvoltage0_1A_phase",
	"axi-ad9144-hpc.out_altvoltage1_1B_phase",
	"axi-ad9144-hpc.out_altvoltage2_2A_phase",
	"axi-ad9144-hpc.out_altvoltage3_2B_phase",
	SYNC_RELOAD,
	NULL,
};

static bool daq2_identify(void)
{
	/* Use the OSC's IIO context just to detect the devices */
	struct iio_context *osc_ctx = get_context_from_osc();
	return !!iio_context_find_device(osc_ctx, "axi-ad9144-hpc") &&
		!!iio_context_find_device(osc_ctx, "axi-ad9680-hpc");
}

struct osc_plugin plugin = {
	.name = "DAQ2",
	.identify = daq2_identify,
	.init = daq2_init,
	.save_restore_attribs = daq2_sr_attribs,
	.handle_item = handle_item,
	.destroy = context_destroy,
};
