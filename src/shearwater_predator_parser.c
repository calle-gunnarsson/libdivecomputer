/*
 * libdivecomputer
 *
 * Copyright (C) 2012 Jef Driesen
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA
 */

#include <stdlib.h>

#include <libdivecomputer/units.h>

#include "shearwater_predator.h"
#include "shearwater_petrel.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define ISINSTANCE(parser)	( \
	dc_parser_isinstance((parser), &shearwater_predator_parser_vtable) || \
	dc_parser_isinstance((parser), &shearwater_petrel_parser_vtable))

#define SZ_BLOCK   0x80
#define SZ_SAMPLE_PREDATOR  0x10
#define SZ_SAMPLE_PETREL    0x20

#define GASSWITCH     0x01
#define PPO2_EXTERNAL 0x02
#define SETPOINT_HIGH 0x04
#define SC            0x08
#define OC            0x10

#define METRIC   0
#define IMPERIAL 1

#define NGASMIXES 10

#define PREDATOR 2
#define PETREL   3

typedef struct shearwater_predator_parser_t shearwater_predator_parser_t;

struct shearwater_predator_parser_t {
	dc_parser_t base;
	unsigned int model;
	unsigned int petrel;
	unsigned int samplesize;
	// Cached fields.
	unsigned int cached;
	unsigned int logversion;
	unsigned int headersize;
	unsigned int footersize;
	unsigned int ngasmixes;
	unsigned int oxygen[NGASMIXES];
	unsigned int helium[NGASMIXES];
	unsigned int calibrated;
	double calibration[3];
	dc_divemode_t mode;
};

static dc_status_t shearwater_predator_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size);
static dc_status_t shearwater_predator_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t shearwater_predator_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t shearwater_predator_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t shearwater_predator_parser_vtable = {
	sizeof(shearwater_predator_parser_t),
	DC_FAMILY_SHEARWATER_PREDATOR,
	shearwater_predator_parser_set_data, /* set_data */
	shearwater_predator_parser_get_datetime, /* datetime */
	shearwater_predator_parser_get_field, /* fields */
	shearwater_predator_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};

static const dc_parser_vtable_t shearwater_petrel_parser_vtable = {
	sizeof(shearwater_predator_parser_t),
	DC_FAMILY_SHEARWATER_PETREL,
	shearwater_predator_parser_set_data, /* set_data */
	shearwater_predator_parser_get_datetime, /* datetime */
	shearwater_predator_parser_get_field, /* fields */
	shearwater_predator_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};


static unsigned int
shearwater_predator_find_gasmix (shearwater_predator_parser_t *parser, unsigned int o2, unsigned int he)
{
	unsigned int i = 0;
	while (i < parser->ngasmixes) {
		if (o2 == parser->oxygen[i] && he == parser->helium[i])
			break;
		i++;
	}

	return i;
}


static dc_status_t
shearwater_common_parser_create (dc_parser_t **out, dc_context_t *context, unsigned int model, unsigned int petrel)
{
	shearwater_predator_parser_t *parser = NULL;
	const dc_parser_vtable_t *vtable = NULL;
	unsigned int samplesize = 0;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	if (petrel) {
		vtable = &shearwater_petrel_parser_vtable;
		samplesize = SZ_SAMPLE_PETREL;
	} else {
		vtable = &shearwater_predator_parser_vtable;
		samplesize = SZ_SAMPLE_PREDATOR;
	}

	// Allocate memory.
	parser = (shearwater_predator_parser_t *) dc_parser_allocate (context, vtable);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	parser->model = model;
	parser->petrel = petrel;
	parser->samplesize = samplesize;
	parser->cached = 0;
	parser->logversion = 0;
	parser->headersize = 0;
	parser->footersize = 0;
	parser->ngasmixes = 0;
	for (unsigned int i = 0; i < NGASMIXES; ++i) {
		parser->oxygen[i] = 0;
		parser->helium[i] = 0;
	}
	parser->calibrated = 0;
	for (unsigned int i = 0; i < 3; ++i) {
		parser->calibration[i] = 0.0;
	}
	parser->mode = DC_DIVEMODE_OC;

	*out = (dc_parser_t *) parser;

	return DC_STATUS_SUCCESS;
}


dc_status_t
shearwater_predator_parser_create (dc_parser_t **out, dc_context_t *context, unsigned int model)
{
	return shearwater_common_parser_create (out, context, model, 0);
}


dc_status_t
shearwater_petrel_parser_create (dc_parser_t **out, dc_context_t *context, unsigned int model)
{
	return shearwater_common_parser_create (out, context, model, 1);
}


static dc_status_t
shearwater_predator_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
	shearwater_predator_parser_t *parser = (shearwater_predator_parser_t *) abstract;

	// Reset the cache.
	parser->cached = 0;
	parser->logversion = 0;
	parser->headersize = 0;
	parser->footersize = 0;
	parser->ngasmixes = 0;
	for (unsigned int i = 0; i < NGASMIXES; ++i) {
		parser->oxygen[i] = 0;
		parser->helium[i] = 0;
	}
	parser->calibrated = 0;
	for (unsigned int i = 0; i < 3; ++i) {
		parser->calibration[i] = 0.0;
	}
	parser->mode = DC_DIVEMODE_OC;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
shearwater_predator_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < 2 * SZ_BLOCK)
		return DC_STATUS_DATAFORMAT;

	unsigned int ticks = array_uint32_be (data + 12);

	if (!dc_datetime_gmtime (datetime, ticks))
		return DC_STATUS_DATAFORMAT;

	datetime->timezone = DC_TIMEZONE_NONE;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
shearwater_predator_parser_cache (shearwater_predator_parser_t *parser)
{
	dc_parser_t *abstract = (dc_parser_t *) parser;
	const unsigned char *data = parser->base.data;
	unsigned int size = parser->base.size;

	if (parser->cached) {
		return DC_STATUS_SUCCESS;
	}

	unsigned int headersize = SZ_BLOCK;
	unsigned int footersize = SZ_BLOCK;
	if (size < headersize + footersize) {
		ERROR (abstract->context, "Invalid data length.");
		return DC_STATUS_DATAFORMAT;
	}

	// Log versions before 6 weren't reliably stored in the data, but
	// 6 is also the oldest version that we assume in our code
	unsigned int logversion = 6;
	if (data[127] > 6)
		logversion = data[127];

	// Adjust the footersize for the final block.
	if (parser->petrel || array_uint16_be (data + size - footersize) == 0xFFFD) {
		footersize += SZ_BLOCK;
		if (size < headersize + footersize) {
			ERROR (abstract->context, "Invalid data length.");
			return DC_STATUS_DATAFORMAT;
		}
	}

	// Default dive mode.
	dc_divemode_t mode = DC_DIVEMODE_OC;

	// Get the gas mixes.
	unsigned int ngasmixes = 0;
	unsigned int oxygen[NGASMIXES] = {0};
	unsigned int helium[NGASMIXES] = {0};
	unsigned int o2_previous = 0, he_previous = 0;

	unsigned int offset = headersize;
	unsigned int length = size - footersize;
	while (offset < length) {
		// Ignore empty samples.
		if (array_isequal (data + offset, parser->samplesize, 0x00)) {
			offset += parser->samplesize;
			continue;
		}

		// Status flags.
		unsigned int status = data[offset + 11];
		if ((status & OC) == 0) {
			mode = DC_DIVEMODE_CCR;
		}

		// Gaschange.
		unsigned int o2 = data[offset + 7];
		unsigned int he = data[offset + 8];
		if (o2 != o2_previous || he != he_previous) {
			// Find the gasmix in the list.
			unsigned int idx = 0;
			while (idx < ngasmixes) {
				if (o2 == oxygen[idx] && he == helium[idx])
					break;
				idx++;
			}

			// Add it to list if not found.
			if (idx >= ngasmixes) {
				if (idx >= NGASMIXES) {
					ERROR (abstract->context, "Maximum number of gas mixes reached.");
					return DC_STATUS_NOMEMORY;
				}
				oxygen[idx] = o2;
				helium[idx] = he;
				ngasmixes = idx + 1;
			}

			o2_previous = o2;
			he_previous = he;
		}

		offset += parser->samplesize;
	}

	// Cache sensor calibration for later use
	unsigned int nsensors = 0, ndefaults = 0;
	for (size_t i = 0; i < 3; ++i) {
		unsigned int calibration = array_uint16_be(data + 87 + i * 2);
		parser->calibration[i] = calibration / 100000.0;
		if (parser->model == PREDATOR) {
			// The Predator expects the mV output of the cells to be
			// within 30mV to 70mV in 100% O2 at 1 atmosphere. If the
			// calibration value is scaled with a factor 2.2, then the
			// sensors lines up and matches the average.
			parser->calibration[i] *= 2.2;
		}
		if (data[86] & (1 << i)) {
			if (calibration == 2100) {
				ndefaults++;
			}
			nsensors++;
		}
	}
	if (nsensors && nsensors == ndefaults) {
		// If all (calibrated) sensors still have their factory default
		// calibration values (2100), they are probably not calibrated
		// properly. To avoid returning incorrect ppO2 values to the
		// application, they are manually disabled (e.g. marked as
		// uncalibrated).
		WARNING (abstract->context, "Disabled all O2 sensors due to a default calibration value.");
		parser->calibrated = 0;
	} else {
		parser->calibrated = data[86];
	}

	// Cache the data for later use.
	parser->logversion = logversion;
	parser->headersize = headersize;
	parser->footersize = footersize;
	parser->ngasmixes = ngasmixes;
	for (unsigned int i = 0; i < ngasmixes; ++i) {
		parser->oxygen[i] = oxygen[i];
		parser->helium[i] = helium[i];
	}
	parser->mode = mode;
	parser->cached = 1;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
shearwater_predator_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	shearwater_predator_parser_t *parser = (shearwater_predator_parser_t *) abstract;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	// Cache the parser data.
	dc_status_t rc = shearwater_predator_parser_cache (parser);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Get the offset to the footer record.
	unsigned int footer = size - parser->footersize;

	// Get the unit system.
	unsigned int units = data[8];

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;
	dc_salinity_t *water = (dc_salinity_t *) value;
	unsigned int density = 0;

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = array_uint16_be (data + footer + 6) * 60;
			break;
		case DC_FIELD_MAXDEPTH:
			if (units == IMPERIAL)
				*((double *) value) = array_uint16_be (data + footer + 4) * FEET;
			else
				*((double *) value) = array_uint16_be (data + footer + 4);
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *) value) = parser->ngasmixes;
			break;
		case DC_FIELD_GASMIX:
			gasmix->oxygen = parser->oxygen[flags] / 100.0;
			gasmix->helium = parser->helium[flags] / 100.0;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		case DC_FIELD_SALINITY:
			density = array_uint16_be (data + 83);
			if (density == 1000)
				water->type = DC_WATER_FRESH;
			else
				water->type = DC_WATER_SALT;
			water->density = density;
			break;
		case DC_FIELD_ATMOSPHERIC:
			*((double *) value) = array_uint16_be (data + 47) / 1000.0;
			break;
		case DC_FIELD_DIVEMODE:
			*((dc_divemode_t *) value) = parser->mode;
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
shearwater_predator_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	shearwater_predator_parser_t *parser = (shearwater_predator_parser_t *) abstract;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	// Cache the parser data.
	dc_status_t rc = shearwater_predator_parser_cache (parser);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Get the unit system.
	unsigned int units = data[8];

	// Previous gas mix.
	unsigned int o2_previous = 0, he_previous = 0;

	unsigned int time = 0;
	unsigned int offset = parser->headersize;
	unsigned int length = size - parser->footersize;
	while (offset < length) {
		dc_sample_value_t sample = {0};

		// Ignore empty samples.
		if (array_isequal (data + offset, parser->samplesize, 0x00)) {
			offset += parser->samplesize;
			continue;
		}

		// Time (seconds).
		time += 10;
		sample.time = time;
		if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

		// Depth (1/10 m or ft).
		unsigned int depth = array_uint16_be (data + offset);
		if (units == IMPERIAL)
			sample.depth = depth * FEET / 10.0;
		else
			sample.depth = depth / 10.0;
		if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);

		// Temperature (°C or °F).
		int temperature = (signed char) data[offset + 13];
		if (temperature < 0) {
			// Fix negative temperatures.
			temperature += 102;
			if (temperature > 0) {
				temperature = 0;
			}
		}
		if (units == IMPERIAL)
			sample.temperature = (temperature - 32.0) * (5.0 / 9.0);
		else
			sample.temperature = temperature;
		if (callback) callback (DC_SAMPLE_TEMPERATURE, sample, userdata);

		// Status flags.
		unsigned int status = data[offset + 11];

		if ((status & OC) == 0) {
			// PPO2
			if ((status & PPO2_EXTERNAL) == 0) {
#ifdef SENSOR_AVERAGE
				sample.ppo2 = data[offset + 6] / 100.0;
				if (callback) callback (DC_SAMPLE_PPO2, sample, userdata);
#else
				sample.ppo2 = data[offset + 12] * parser->calibration[0];
				if (callback && (parser->calibrated & 0x01)) callback (DC_SAMPLE_PPO2, sample, userdata);

				sample.ppo2 = data[offset + 14] * parser->calibration[1];
				if (callback && (parser->calibrated & 0x02)) callback (DC_SAMPLE_PPO2, sample, userdata);

				sample.ppo2 = data[offset + 15] * parser->calibration[2];
				if (callback && (parser->calibrated & 0x04)) callback (DC_SAMPLE_PPO2, sample, userdata);
#endif
			}

			// Setpoint
			if (parser->petrel) {
				sample.setpoint = data[offset + 18] / 100.0;
			} else {
				if (status & SETPOINT_HIGH) {
					sample.setpoint = data[18] / 100.0;
				} else {
					sample.setpoint = data[17] / 100.0;
				}
			}
			if (callback) callback (DC_SAMPLE_SETPOINT, sample, userdata);
		}

		// CNS
		if (parser->petrel) {
			sample.cns = data[offset + 22] / 100.0;
			if (callback) callback (DC_SAMPLE_CNS, sample, userdata);
		}

		// Gaschange.
		unsigned int o2 = data[offset + 7];
		unsigned int he = data[offset + 8];
		if (o2 != o2_previous || he != he_previous) {
			unsigned int idx = shearwater_predator_find_gasmix (parser, o2, he);
			if (idx >= parser->ngasmixes) {
				ERROR (abstract->context, "Invalid gas mix.");
				return DC_STATUS_DATAFORMAT;
			}

			sample.gasmix = idx;
			if (callback) callback (DC_SAMPLE_GASMIX, sample, userdata);
			o2_previous = o2;
			he_previous = he;
		}

		// Deco stop / NDL.
		unsigned int decostop = array_uint16_be (data + offset + 2);
		if (decostop) {
			sample.deco.type = DC_DECO_DECOSTOP;
			if (units == IMPERIAL)
				sample.deco.depth = decostop * FEET;
			else
				sample.deco.depth = decostop;
		} else {
			sample.deco.type = DC_DECO_NDL;
			sample.deco.depth = 0.0;
		}
		sample.deco.time = data[offset + 9] * 60;
		if (callback) callback (DC_SAMPLE_DECO, sample, userdata);

		// for logversion 7 and newer (introduced for Perdix AI)
		// detect tank pressure
		if (parser->logversion >= 7) {
			// Tank pressure
			// Values above 0xFFF0 are special codes:
			//    0xFFFF AI is off
			//    0xFFFE No comms for 90 seconds+
			//    0xFFFD No comms for 30 seconds
			//    0xFFFC Transmitter not paired
			// For regular values, the top 4 bits contain the battery
			// level (0=normal, 1=critical, 2=warning), and the lower 12
			// bits the tank pressure in units of 2 psi.
			unsigned int pressure = array_uint16_be (data + offset + 27);
			if (pressure < 0xFFF0) {
				pressure &= 0x0FFF;
				sample.pressure.tank = 0;
				sample.pressure.value = pressure * 2 * PSI / BAR;
				if (callback) callback (DC_SAMPLE_PRESSURE, sample, userdata);
			}
			pressure = array_uint16_be (data + offset + 19);
			if (pressure < 0xFFF0) {
				pressure &= 0x0FFF;
				sample.pressure.tank = 1;
				sample.pressure.value = pressure * 2 * PSI / BAR;
				if (callback) callback (DC_SAMPLE_PRESSURE, sample, userdata);
			}

			// Gas time remaining in minutes
			// Values above 0xF0 are special codes:
			//    0xFF Not paired
			//    0xFE No communication
			//    0xFD Not available in current mode
			//    0xFC Not available because of DECO
			//    0xFB Tank size or max pressure haven’t been set up
			if (data[offset + 21] < 0xF0) {
				sample.rbt = data[offset + 21];
				if (callback) callback (DC_SAMPLE_RBT, sample, userdata);
			}
		}

		offset += parser->samplesize;
	}

	return DC_STATUS_SUCCESS;
}
