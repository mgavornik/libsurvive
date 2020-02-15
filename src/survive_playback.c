// All MIT/x11 Licensed Code in this file may be relicensed freely under the GPL
// or LGPL licenses.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <survive.h>

#include <assert.h>
#include <errno.h>
#include <string.h>

#ifdef NOZLIB
#define gzFile FILE *
#define gzopen fopen
#define gzprintf fprintf
#define gzclose fclose
#define gzvprintf vfprintf
#define gzerror_dropin ferror
#define gzwrite write
#define gzeof feof
#define gzseek fseek
#define gzgetc fgetc
#else
#include <zlib.h>
int gzerror_dropin(gzFile f) {
	int rtn;
	gzerror(f, &rtn);
	return rtn;
}
#endif

#include "survive_config.h"
#include "survive_default_devices.h"

#include "ctype.h"
#include "os_generic.h"
#include "stdarg.h"

#ifdef _MSC_VER
typedef long ssize_t;
#define SSIZE_MAX LONG_MAX

ssize_t getdelim(char **lineptr, size_t *n, int delimiter, FILE *stream);
ssize_t getline(char **lineptr, size_t *n, FILE *stream);
#define RESTRICT_KEYWORD
#else
#define RESTRICT_KEYWORD restrict
#endif

ssize_t gzgetdelim(char **RESTRICT_KEYWORD lineptr, size_t *RESTRICT_KEYWORD n, int delimiter,
				   gzFile RESTRICT_KEYWORD stream);
ssize_t gzgetline(char **RESTRICT_KEYWORD lineptr, size_t *RESTRICT_KEYWORD n, gzFile RESTRICT_KEYWORD stream);

STATIC_CONFIG_ITEM(PLAYBACK_REPLAY_POSE, "playback-replay-pose", 'i', "Whether or not to output pose", 0);
STATIC_CONFIG_ITEM( RECORD, "record", 's', "File to record to if you wish to make a recording.", "" );
STATIC_CONFIG_ITEM(RECORD_STDOUT, "record-stdout", 'i', "Whether or not to dump recording data to stdout", 0);
STATIC_CONFIG_ITEM( PLAYBACK, "playback", 's', "File to be used for playback if playing a recording.", "" );
STATIC_CONFIG_ITEM( PLAYBACK_FACTOR, "playback-factor", 'f', "Time factor of playback -- 1 is run at the same timing as original, 0 is run as fast as possible.", 1.0f );
STATIC_CONFIG_ITEM( PLAYBACK_RECORD_RAWLIGHT, "record-rawlight", 'i', "Whether or not to output raw light data", 1 );
STATIC_CONFIG_ITEM( PLAYBACK_RECORD_IMU, "record-imu", 'i', "Whether or not to output imu data", 1 );
STATIC_CONFIG_ITEM(PLAYBACK_RECORD_CAL_IMU, "record-cal-imu", 'i', "Whether or not to output calibrated imu data", 0);
STATIC_CONFIG_ITEM( PLAYBACK_RECORD_ANGLE, "record-angle", 'i', "Whether or not to output angle data", 1 );

typedef struct SurviveRecordingData {
	SurviveContext *ctx;
	bool alwaysWriteStdOut;
	bool writeRawLight;
        bool writeIMU;
		bool writeCalIMU;
		bool writeAngle;
		gzFile output_file;
} SurviveRecordingData;

struct SurvivePlaybackData {
	SurviveContext *ctx;
	const char *playback_dir;
	gzFile playback_file;
	int lineno;

	double next_time_s;
	double time_now;
	FLT playback_factor;
	bool hasRawLight;
	bool outputExternalPose;

	uint32_t total_sleep_time;
	bool keepRunning;
	og_thread_t playback_thread;
};

static double timestamp_in_s() {
	static double start_time_s = 0;
	if (start_time_s == 0.)
		start_time_s = OGGetAbsoluteTime();
	return OGGetAbsoluteTime() - start_time_s;
}

static int playback_poll(struct SurviveContext *ctx, void *_driver);
static double survive_usbmon_playback_run_time(const SurviveContext *ctx, void *_sp) {
	const struct SurvivePlaybackData *sp = _sp;
	return sp->time_now;
}

static void write_to_output_raw(SurviveRecordingData *recordingData, const char *string, int len) {
	if (recordingData->output_file) {
		//gzwrite(recordingData->output_file, string, len);
		fwrite(string,1, len, recordingData->output_file);
	}

	if (recordingData->alwaysWriteStdOut) {
		fwrite(string, 1, len, stdout);
	}
}

static void write_to_output(SurviveRecordingData *recordingData, const char *format, ...) {
	if (!recordingData) {
		return;
	}

	double ts = survive_run_time(recordingData->ctx);

	if (recordingData->output_file) {
		va_list args;
		va_start(args, format);
		gzprintf(recordingData->output_file, "%0.6f ", ts);
		gzvprintf(recordingData->output_file, format, args);

		va_end(args);
	}

	if (recordingData->alwaysWriteStdOut) {
		va_list args;
		va_start(args, format);
		fprintf(stdout, "%0.6f ", ts);
		vfprintf(stdout, format, args);
		va_end(args);
	}
}
void survive_recording_config_process(SurviveObject *so, char *ct0conf, int len) {
	SurviveRecordingData *recordingData = so->ctx->recptr;
	if (recordingData == 0)
		return;

	char *buffer = SV_CALLOC(1, len + 1);
	memcpy(buffer, ct0conf, len);
	for (int i = 0; i < len; i++)
		if (buffer[i] == '\n')
			buffer[i] = ' ';

	write_to_output(recordingData, "%s CONFIG ", so->codename);
	write_to_output_raw(recordingData, buffer, len);
	write_to_output_raw(recordingData, "\n", 1);
	free(buffer);
}

void survive_recording_lighthouse_process(SurviveContext *ctx, uint8_t lighthouse, SurvivePose *lh_pose,
										  SurvivePose *obj) {
	SurviveRecordingData *recordingData = ctx->recptr;
	if (recordingData == 0)
		return;

	write_to_output(recordingData, "%d LH_POSE %0.6f %0.6f %0.6f %0.6f %0.6f %0.6f %0.6f\n", lighthouse,
					lh_pose->Pos[0], lh_pose->Pos[1], lh_pose->Pos[2], lh_pose->Rot[0], lh_pose->Rot[1],
					lh_pose->Rot[2], lh_pose->Rot[3]);
}
void survive_recording_velocity_process(SurviveObject *so, uint8_t lighthouse, const SurviveVelocity *pose) {
	SurviveRecordingData *recordingData = so->ctx->recptr;
	if (recordingData == 0)
		return;

	write_to_output(recordingData, "%s VELOCITY %0.6f %0.6f %0.6f %0.6f %0.6f %0.6f\n", so->codename, pose->Pos[0],
					pose->Pos[1], pose->Pos[2], pose->AxisAngleRot[0], pose->AxisAngleRot[1], pose->AxisAngleRot[2]);
}
void survive_recording_raw_pose_process(SurviveObject *so, uint8_t lighthouse, SurvivePose *pose) {
	SurviveRecordingData *recordingData = so->ctx->recptr;
	if (recordingData == 0)
		return;

	write_to_output(recordingData, "%s POSE %0.6f %0.6f %0.6f %0.6f %0.6f %0.6f %0.6f\n", so->codename, pose->Pos[0],
					pose->Pos[1], pose->Pos[2], pose->Rot[0], pose->Rot[1], pose->Rot[2], pose->Rot[3]);
}

void survive_recording_external_velocity_process(SurviveContext *ctx, const char *name, const SurviveVelocity *pose) {
	SurviveRecordingData *recordingData = ctx->recptr;
	if (recordingData == 0)
		return;

	write_to_output(recordingData, "%s EXTERNAL_VELOCITY %0.6f %0.6f %0.6f %0.6f %0.6f %0.6f\n", name, pose->Pos[0],
					pose->Pos[1], pose->Pos[2], pose->AxisAngleRot[0], pose->AxisAngleRot[1], pose->AxisAngleRot[2]);
}

void survive_recording_external_pose_process(SurviveContext *ctx, const char *name, const SurvivePose *pose) {
	SurviveRecordingData *recordingData = ctx->recptr;
	if (recordingData == 0)
		return;

	write_to_output(recordingData, "%s EXTERNAL_POSE %0.6f %0.6f %0.6f %0.6f %0.6f %0.6f %0.6f\n", name, pose->Pos[0],
					pose->Pos[1], pose->Pos[2], pose->Rot[0], pose->Rot[1], pose->Rot[2], pose->Rot[3]);
}

void survive_recording_info_process(SurviveContext *ctx, const char *fault) {
	SurviveRecordingData *recordingData = ctx->recptr;
	if (recordingData == 0)
		return;

	write_to_output(recordingData, "INFO LOG %s\n", fault);
}

// survive_channel channel, int sensor_id, survive_timecode timecode, int8_t plane, FLT angle
#define SWEEP_ANGLE_SCANF "%s B %hhu %u %u %hhu " FLT_sformat "\n"
#define SWEEP_ANGLE_PRINTF "%s B %hhu %u %u %hhu " FLT_format "\n"
#define SWEEP_ANGLE_SCANF_ARGS dev, &channel, &sensor_id, &timecode, &plane, &angle
#define SWEEP_ANGLE_PRINTF_ARGS dev, channel, sensor_id, timecode, plane, angle

#define SWEEP_SCANF "%s W %hhu %u %u %hhu\n"
#define SWEEP_PRINTF SWEEP_SCANF
#define SWEEP_SCANF_ARGS dev, &channel, &sensor_id, &timecode, &flag
#define SWEEP_PRINTF_ARGS dev, channel, sensor_id, timecode, flag

#define SYNC_SCANF "%s Y %hhu %u %hhu %hhu\n"
#define SYNC_PRINTF SYNC_SCANF
#define SYNC_SCANF_ARGS dev, &channel, &timecode, &ootx, &gen
#define SYNC_PRINTF_ARGS dev, channel, timecode, ootx, gen

void survive_recording_sync_process(SurviveObject *so, survive_channel channel, survive_timecode timecode, bool ootx,
									bool gen) {
	SurviveRecordingData *recordingData = so->ctx->recptr;
	const char *dev = so->codename;
	write_to_output(recordingData, SYNC_PRINTF, SYNC_PRINTF_ARGS);
};

void survive_recording_sweep_angle_process(SurviveObject *so, survive_channel channel, int sensor_id,
										   survive_timecode timecode, int8_t plane, FLT angle) {
	SurviveRecordingData *recordingData = so->ctx->recptr;
	if (recordingData == 0)
		return;

	if (!recordingData->writeAngle)
		return;

	const char *dev = so->codename;
	write_to_output(recordingData, SWEEP_ANGLE_PRINTF, SWEEP_ANGLE_PRINTF_ARGS);
}

void survive_recording_sweep_process(SurviveObject *so, survive_channel channel, int sensor_id,
									 survive_timecode timecode, bool flag) {
	SurviveRecordingData *recordingData = so->ctx->recptr;
	if (recordingData == 0)
		return;

	const char *dev = so->codename;
	write_to_output(recordingData, SWEEP_PRINTF, SWEEP_PRINTF_ARGS);
}
void survive_recording_angle_process(struct SurviveObject *so, int sensor_id, int acode, uint32_t timecode, FLT length,
									 FLT angle, uint32_t lh) {
	SurviveRecordingData *recordingData = so->ctx->recptr;
	if (recordingData == 0)
		return;

	if (!recordingData->writeAngle) {
	  return;
	}

	write_to_output(recordingData, "%s A %d %d %u %0.6f %0.6f %u\n", so->codename, sensor_id, acode, timecode, length,
					angle, lh);
}

void survive_recording_lightcap(SurviveObject *so, LightcapElement *le) {
	SurviveRecordingData *recordingData = so->ctx->recptr;
	if (recordingData == 0)
		return;

	if (recordingData->writeRawLight) {
		write_to_output(recordingData, "%s C %d %u %u\n", so->codename, le->sensor_id, le->timestamp, le->length);
	}
}

void survive_recording_light_process(struct SurviveObject *so, int sensor_id, int acode, int timeinsweep,
									 uint32_t timecode, uint32_t length, uint32_t lh) {
	SurviveRecordingData *recordingData = so->ctx->recptr;
	if (recordingData == 0)
		return;

	if (!recordingData->writeAngle) {
	  return;
	}
	
	if (acode == -1) {
		write_to_output(recordingData, "%s S %d %d %d %u %u %u\n", so->codename, sensor_id, acode, timeinsweep,
						timecode, length, lh);
		return;
	}

	const char *LH_ID = 0;
	const char *LH_Axis = 0;

	switch (acode) {
	case 0:
	case 2:
		LH_ID = "L";
		LH_Axis = "X";
		break;
	case 1:
	case 3:
		LH_ID = "L";
		LH_Axis = "Y";
		break;
	case 4:
	case 6:
		LH_ID = "R";
		LH_Axis = "X";
		break;
	case 5:
	case 7:
		LH_ID = "R";
		LH_Axis = "Y";
		break;
	}

	write_to_output(recordingData, "%s %s %s %d %d %d %u %u %u\n", so->codename, LH_ID, LH_Axis, sensor_id, acode,
					timeinsweep, timecode, length, lh);
}

void survive_recording_imu_process(struct SurviveObject *so, int mask, FLT *accelgyro, uint32_t timecode, int id) {
	SurviveRecordingData *recordingData = so->ctx->recptr;
	if (recordingData == 0)
		return;

	if (!recordingData->writeCalIMU) {
		return;
	}
	
	write_to_output(recordingData, "%s I %d %u %0.6f %0.6f %0.6f %0.6f %0.6f %0.6f  %0.6f %0.6f %0.6f %d\n",
					so->codename, mask, timecode, accelgyro[0], accelgyro[1], accelgyro[2], accelgyro[3], accelgyro[4],
					accelgyro[5], accelgyro[6], accelgyro[7], accelgyro[8], id);
}

void survive_recording_raw_imu_process(struct SurviveObject *so, int mask, FLT *accelgyro, uint32_t timecode, int id) {
	SurviveRecordingData *recordingData = so->ctx->recptr;
	if (recordingData == 0)
		return;

	if (!recordingData->writeIMU) {
		return;
	}

	write_to_output(recordingData, "%s i %d %u %0.6f %0.6f %0.6f %0.6f %0.6f %0.6f  %0.6f %0.6f %0.6f %d\n",
					so->codename, mask, timecode, accelgyro[0], accelgyro[1], accelgyro[2], accelgyro[3], accelgyro[4],
					accelgyro[5], accelgyro[6], accelgyro[7], accelgyro[8], id);
}

typedef struct SurvivePlaybackData SurvivePlaybackData;

static SurviveObject *find_or_warn(SurvivePlaybackData *driver, const char *dev) {
	SurviveContext *ctx = driver->ctx;
	SurviveObject *so = survive_get_so_by_name(driver->ctx, dev);
	if (!so) {
		static bool display_once = false;
		SurviveContext *ctx = driver->ctx;
		if (display_once == false) {
			SV_ERROR(SURVIVE_ERROR_INVALID_CONFIG, "Could not find device named %s from lineno %d\n", dev,
					 driver->lineno);
		}
		display_once = true;

		return 0;
	}
	return so;
}

static int parse_and_run_sweep(char *line, SurvivePlaybackData *driver) {
	char dev[10];

	survive_channel channel;
	int sensor_id;
	survive_timecode timecode;
	uint8_t flag;

	int rr = sscanf(line, SWEEP_SCANF, SWEEP_SCANF_ARGS);
	if (rr != 5) {
		SurviveContext *ctx = driver->ctx;
		SV_WARN("Only got %d values for a sweep", rr);
		return -1;
	}

	SurviveObject *so = find_or_warn(driver, dev);
	if (!so) {
		return -1;
	}

	driver->ctx->sweepproc(so, channel, sensor_id, timecode, flag);
	return 0;
}

static int parse_and_run_sync(char *line, SurvivePlaybackData *driver) {
	char dev[10];

	survive_channel channel;
	survive_timecode timecode;
	uint8_t ootx, gen;

	int rr = sscanf(line, SYNC_SCANF, SYNC_SCANF_ARGS);
	if (rr != 5) {
		SurviveContext *ctx = driver->ctx;
		SV_WARN("Only got %d values for a sync", rr);
		return -1;
	}

	SurviveObject *so = find_or_warn(driver, dev);
	if (!so) {
		return -1;
	}

	driver->ctx->syncproc(so, channel, timecode, ootx, gen);
	return 0;
}

static int parse_and_run_sweep_angle(char *line, SurvivePlaybackData *driver) {
	char dev[10];

	survive_channel channel;
	int sensor_id;
	survive_timecode timecode;
	int8_t plane;
	FLT angle;

	int rr = sscanf(line, SWEEP_ANGLE_SCANF, SWEEP_ANGLE_SCANF_ARGS);

	if (rr != 6) {
		SurviveContext *ctx = driver->ctx;
		SV_WARN("Only got %d values for sweep angle", rr);
		return -1;
	}

	SurviveObject *so = find_or_warn(driver, dev);
	if (!so) {
		return -1;
	}

	driver->ctx->sweep_angleproc(so, channel, sensor_id, timecode, plane, angle);
	return 0;
}

static int parse_and_run_pose(const char *line, SurvivePlaybackData *driver) {
	char name[128] = "replay_";
	SurvivePose pose;

	int rr = sscanf(line, "%s POSE " SurvivePose_sformat "\n", name + strlen(name), &pose.Pos[0], &pose.Pos[1],
					&pose.Pos[2], &pose.Rot[0], &pose.Rot[1], &pose.Rot[2], &pose.Rot[3]);

	SurviveContext *ctx = driver->ctx;
	if (rr != 8) {
		SV_WARN("Only got %d values for a pose", rr);
		return 0;
	}

	ctx->external_poseproc(ctx, name, &pose);
	return 0;
}
static int parse_and_run_imu(const char *line, SurvivePlaybackData *driver, bool raw) {
	char dev[10];
	int timecode = 0;
	FLT accelgyro[9] = { 0 };
	int mask;
	int id;
	SurviveContext *ctx = driver->ctx;

	char i_char = 0;

	int rr = sscanf(line,
					"%s %c %d %d " FLT_sformat " " FLT_sformat " " FLT_sformat " " FLT_sformat " " FLT_sformat
					" " FLT_sformat " " FLT_sformat " " FLT_sformat " " FLT_sformat "%d",
					dev, &i_char, &mask, &timecode, &accelgyro[0], &accelgyro[1], &accelgyro[2], &accelgyro[3],
					&accelgyro[4], &accelgyro[5], &accelgyro[6], &accelgyro[7], &accelgyro[8], &id);

	if (rr == 11) {
		// Older formats might not have mag data
		id = accelgyro[6];
		accelgyro[6] = 0;
	} else if (rr != 14) {
		SV_WARN("On line %d, only %d values read: '%s'", driver->lineno, rr, line);
		return -1;
	}

	assert(raw ^ i_char == 'I');

	SurviveObject *so = survive_get_so_by_name(driver->ctx, dev);
	if (!so) {
		static bool display_once = false;
		if (display_once == false) {
			SV_ERROR(SURVIVE_ERROR_INVALID_CONFIG, "Could not find device named %s from lineno %d\n", dev,
					 driver->lineno);
		}
		display_once = true;
		return -1;
	}

	(raw ? driver->ctx->raw_imuproc : driver->ctx->imuproc)(so, mask, accelgyro, timecode, id);
	return 0;
}

static int parse_and_run_externalpose(const char *line, SurvivePlaybackData *driver) {
	char name[128] = { 0 };
	SurvivePose pose;

	int rr = sscanf(line, "%s EXTERNAL_POSE " SurvivePose_sformat "\n", name, &pose.Pos[0], &pose.Pos[1], &pose.Pos[2],
					&pose.Rot[0], &pose.Rot[1], &pose.Rot[2], &pose.Rot[3]);

	SurviveContext *ctx = driver->ctx;
	ctx->external_poseproc(ctx, name, &pose);
	return 0;
}

static int parse_and_run_rawlight(const char *line, SurvivePlaybackData *driver) {
	driver->hasRawLight = 1;

	char dev[10];
	char op[10];
	LightcapElement le;
	int rr = sscanf(line, "%s %s %hhu %u %hu\n", dev, op, &le.sensor_id, &le.timestamp, &le.length);

	SurviveObject *so = survive_get_so_by_name(driver->ctx, dev);
	if (!so) {
		static bool display_once = false;
		SurviveContext *ctx = driver->ctx;
		if (display_once == false) {
			SV_ERROR(SURVIVE_ERROR_INVALID_CONFIG, "Could not find device named %s from lineno %d\n", dev,
					 driver->lineno);
		}
		display_once = true;

		return -1;
	}

	handle_lightcap(so, &le);
	return 0;
}

static int parse_and_run_lightcode(const char *line, SurvivePlaybackData *driver) {
	char lhn[10];
	char axn[10];
	char dev[10];
	uint32_t timecode = 0;
	int sensor_id = 0;
	int acode = 0;
	int timeinsweep = 0;
	uint32_t length = 0;
	uint32_t lh = 0;
	SurviveContext *ctx = driver->ctx;

	int rr = sscanf(line, "%8s %8s %8s %u %d %d %d %u %u\n", dev, lhn, axn, &sensor_id, &acode, &timeinsweep, &timecode,
					&length, &lh);

	if (rr != 9) {
		SV_WARN("Warning:  On line %d, only %d values read: '%s'\n", driver->lineno, rr, line);
		return -1;
	}

	SurviveObject *so = survive_get_so_by_name(driver->ctx, dev);
	if (!so) {
		static bool display_once = false;
		if (display_once == false) {
			SV_ERROR(SURVIVE_ERROR_INVALID_CONFIG, "Could not find device named %s from lineno %d\n", dev,
					 driver->lineno);
		}
		display_once = true;

		return -1;
	}

	driver->ctx->lightproc(so, sensor_id, acode, timeinsweep, timecode, length, lh);
	return 0;
}

static int playback_pump_msg(struct SurviveContext *ctx, void *_driver) {
	SurvivePlaybackData *driver = _driver;
	gzFile f = driver->playback_file;

	if (f && !gzeof(f) && !gzerror_dropin(f)) {
		driver->lineno++;
		char *line = 0;

		if (driver->next_time_s == 0) {
			size_t n = 0;
			ssize_t r = gzgetdelim(&line, &n, ' ', f);
			if (r <= 0) {
				return 0;
			}

			if (sscanf(line, "%lf", &driver->next_time_s) != 1) {
				free(line);
				return 0;
			}
			free(line);
			line = 0;
		}

		if (driver->next_time_s * driver->playback_factor > timestamp_in_s())
			return 0;

		driver->time_now = driver->next_time_s;
		driver->next_time_s = 0;

		size_t n = 0;
		ssize_t r = gzgetline(&line, &n, f);
		if (r <= 0) {
			free(line);
			return 0;
		}
		while (r && (line[r - 1] == '\n' || line[r - 1] == '\r')) {
			line[--r] = 0;
		}
		char dev[32];
		char op[32];
		if (sscanf(line, "%31s %31s", dev, op) < 2) {
			free(line);
			return 0;
		}

		survive_get_ctx_lock(ctx);
		switch (op[0]) {
		case 'W':
			if (op[1] == 0)
				parse_and_run_sweep(line, driver);
			break;
		case 'B':
			if (op[1] == 0)
				parse_and_run_sweep_angle(line, driver);
			break;
		case 'Y':
			if (op[1] == 0)
				parse_and_run_sync(line, driver);
			break;
		case 'E':
			if (strcmp(op, "EXTERNAL_POSE") == 0) {
				parse_and_run_externalpose(line, driver);
				break;
			}
		case 'C':
			if (op[1] == 0)
				parse_and_run_rawlight(line, driver);
			break;
		case 'L':
		case 'R':
			if (op[1] == 0 && driver->hasRawLight == false)
				parse_and_run_lightcode(line, driver);
			break;
		case 'i':
			if (op[1] == 0)
				parse_and_run_imu(line, driver, true);
			break;
		case 'I':
			if (op[1] == 0)
				parse_and_run_imu(line, driver, false);
			break;
		case 'P':
			if (strcmp(op, "POSE") == 0 && driver->outputExternalPose)
				parse_and_run_pose(line, driver);
			break;
		case 'A':
		case 'V':
			break;
		default:
			SV_WARN("Playback doesn't understand '%s' op in '%s'", op, line);
		}
		survive_release_ctx_lock(ctx);

		free(line);
	} else {
		if (f) {
			gzclose(driver->playback_file);
		}
		driver->playback_file = 0;
		return -1;
	}

	return 0;
}

static void *playback_thread(void *_driver) {
	SurvivePlaybackData *driver = _driver;
	driver->keepRunning = true;
	while (driver->keepRunning) {
		double next_time_s_scaled = driver->next_time_s * driver->playback_factor;
		double time_now = timestamp_in_s();
		if (next_time_s_scaled == 0 || next_time_s_scaled < time_now) {
			int rtnVal = playback_pump_msg(driver->ctx, driver);
			if (rtnVal < 0)
				driver->keepRunning = false;
		} else {
			int sleep_time_ms = 1 + (next_time_s_scaled - time_now) * 1000.;
			int sr = OGUSleep(sleep_time_ms * 1000);
			if (sr == 0)
				driver->total_sleep_time += sleep_time_ms;
		}
	}
	return 0;
}

static int playback_poll(struct SurviveContext *ctx, void *_driver) {
	SurvivePlaybackData *driver = _driver;
	if (driver->keepRunning == false)
		return -1;
	return 0;
}

static int playback_close(struct SurviveContext *ctx, void *_driver) {
	SurvivePlaybackData *driver = _driver;
	driver->keepRunning = false;
	SV_VERBOSE(100, "Waiting on playback thread...");
	survive_release_ctx_lock(ctx);
	OGJoinThread(driver->playback_thread);
	survive_get_ctx_lock(ctx);
	SV_VERBOSE(100, "Playback thread slept for %ums", driver->total_sleep_time);
	if (driver->playback_file)
		gzclose(driver->playback_file);
	driver->playback_file = 0;

	survive_detach_config(ctx, "playback-factor", &driver->playback_factor);

	free(driver);
	return 0;
}

void survive_destroy_recording(SurviveContext *ctx) {
	if (ctx->recptr) {
		gzclose(ctx->recptr->output_file);
		free(ctx->recptr);
		ctx->recptr = 0;
	}
}

void survive_install_recording(SurviveContext *ctx) {
	const char *dataout_file = survive_configs(ctx, "record", SC_GET, "");
	int record_to_stdout = survive_configi(ctx, "record-stdout", SC_GET, 0);

	if (strlen(dataout_file) > 0 || record_to_stdout) {
		ctx->recptr = SV_CALLOC(1, sizeof(struct SurviveRecordingData));
		ctx->recptr->ctx = ctx;
		if (strlen(dataout_file) > 0) {
			bool useCompression = strncmp(dataout_file + strlen(dataout_file) - 3, ".gz", 3) == 0;

			ctx->recptr->output_file = gzopen(dataout_file, useCompression ? "w" : "wT");
			if (ctx->recptr->output_file == 0) {
				SV_INFO("Could not open %s for writing", dataout_file);
				free(ctx->recptr);
				ctx->recptr = 0;
				return;
			}
			SV_INFO("Recording to '%s' Compression: %d", dataout_file, useCompression);
		}

		ctx->recptr->alwaysWriteStdOut = record_to_stdout;
		if (record_to_stdout) {
			SV_INFO("Recording to stdout");
		}

		ctx->recptr->writeRawLight = survive_configi(ctx, "record-rawlight", SC_GET, 1);
		ctx->recptr->writeIMU = survive_configi(ctx, "record-imu", SC_GET, 1);
		ctx->recptr->writeCalIMU = survive_configi(ctx, "record-cal-imu", SC_GET, 0);
		ctx->recptr->writeAngle = survive_configi(ctx, "record-angle", SC_GET, 1);
	}
}

int DriverRegPlayback(SurviveContext *ctx) {
	const char *playback_file = survive_configs(ctx, "playback", SC_GET, "");

	if (strlen(playback_file) == 0) {
		SV_WARN("The playback argument requires a filename");
		return -1;
	}

	SurvivePlaybackData *sp = SV_CALLOC(1, sizeof(SurvivePlaybackData));
	sp->ctx = ctx;
	sp->playback_dir = playback_file;

	sp->outputExternalPose = survive_configi(ctx, "playback-replay-pose", SC_GET, 0);

	sp->playback_file = gzopen(playback_file, "r");
	if (sp->playback_file == 0) {
		SV_ERROR(SURVIVE_ERROR_INVALID_CONFIG, "Could not open playback events file %s", playback_file);
		return -1;
	}
	survive_install_run_time_fn(ctx, survive_usbmon_playback_run_time, sp);
	survive_attach_configf(ctx, "playback-factor", &sp->playback_factor);

	SV_INFO("Using playback file '%s' with timefactor of %f", playback_file, sp->playback_factor);

	ctx->poll_min_time_ms = 1;
	if (sp->playback_factor == 0.0)
		ctx->poll_min_time_ms = 0;

	FLT time;
	while (!gzeof(sp->playback_file) && !gzerror_dropin(sp->playback_file)) {
		char *line = 0;
		size_t n;
		int r = gzgetline(&line, &n, sp->playback_file);

		if (r <= 0) {
			free(line);
			continue;
		}

		if (line[0] == 0x1f) {
			SV_ERROR(SURVIVE_ERROR_INVALID_CONFIG, "Attempting to playback a gz compressed file without gz support.");
			free(line);
			return -1;
		}

		char dev[32];
		char command[32];

		if (sscanf(line, "%lf %s %s", &time, dev, command) != 3) {
			free(line);
			break;
		}

		// 10 seconds is enough time for all configurations; don't read the whole file -- could be huge
		if (time > 10) {
			free(line);
			break;
		}

		if (strcmp(command, "CONFIG") == 0) {
			char *configStart = line;

			// Skip three spaces
			for (int i = 0; i < 3; i++) {
				while (*(++configStart) != ' ')
					;
			}
			size_t len = strlen(configStart);

			SurviveObject *so = survive_create_device(ctx, "replay", sp, dev, 0);

			char *config = SV_CALLOC(1, len + 1);
			memcpy(config, configStart, len);

			if (ctx->configproc(so, config, len) == 0) {
				SV_INFO("Found %s in playback file...", dev);
				survive_add_object(ctx, so);
			} else {
				SV_WARN("Found %s in playback file, but could not read config description", dev);
				free(so);
			}
		}

		free(line);
	}

	gzseek(sp->playback_file, 0, SEEK_SET); // same as rewind(f);

	sp->playback_thread = OGCreateThread(playback_thread, sp);
	OGNameThread(sp->playback_thread, "playback");
	survive_add_driver(ctx, sp, playback_poll, playback_close, 0);
	return 0;
}

REGISTER_LINKTIME(DriverRegPlayback);

#define _GETDELIM_GROWBY 128 /* amount to grow line buffer by */
#define _GETDELIM_MINLEN 4   /* minimum line buffer size */

ssize_t gzgetdelim(char **RESTRICT_KEYWORD lineptr, size_t *RESTRICT_KEYWORD n, int delimiter,
				   gzFile RESTRICT_KEYWORD stream) {
	char *buf, *pos;
	int c;
	ssize_t bytes;

	if (lineptr == NULL || n == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (stream == NULL) {
		errno = EBADF;
		return -1;
	}

	/* resize (or allocate) the line buffer if necessary */
	buf = *lineptr;
	if (buf == NULL || *n < _GETDELIM_MINLEN) {
		buf = SV_REALLOC(*lineptr, _GETDELIM_GROWBY);
		if (buf == NULL) {
			/* ENOMEM */
			return -1;
		}
		*n = _GETDELIM_GROWBY;
		*lineptr = buf;
	}

	/* read characters until delimiter is found, end of file is reached, or an
	   error occurs. */
	bytes = 0;
	pos = buf;
	while ((c = gzgetc(stream)) != EOF) {
		if (bytes + 1 >= SSIZE_MAX) {
			errno = EOVERFLOW;
			return -1;
		}
		bytes++;
		if (bytes >= *n - 1) {
			buf = SV_REALLOC(*lineptr, *n + _GETDELIM_GROWBY);
			if (buf == NULL) {
				/* ENOMEM */
				return -1;
			}
			*n += _GETDELIM_GROWBY;
			pos = buf + bytes - 1;
			*lineptr = buf;
		}

		*pos++ = (char)c;
		if (c == delimiter) {
			break;
		}
	}

	if (gzerror_dropin(stream) || (gzeof(stream) && (bytes == 0))) {
		/* EOF, or an error from getc(). */
		return -1;
	}

	*pos = '\0';
	return bytes;
}

ssize_t gzgetline(char **RESTRICT_KEYWORD lineptr, size_t *RESTRICT_KEYWORD n, gzFile RESTRICT_KEYWORD stream) {
	return gzgetdelim(lineptr, n, '\n', stream);
}
