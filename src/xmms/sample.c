/*  XMMS2 - X Music Multiplexer System
 *  Copyright (C) 2003	Peter Alm, Tobias Rundstr�m, Anders Gustafsson
 * 
 *  PLUGINS ARE NOT CONSIDERED TO BE DERIVED WORK !!!
 * 
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *                   
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 */


#include <glib.h>
#include <math.h>
#include "xmms/object.h"
#include "xmms/sample.h"
#include "xmms/util.h"

/**
  * @defgroup Sample Sample Converter
  * @ingroup XMMSServer
  * @brief Convert sample formats back and forth.
  * @{ 
  */

/**
 * The converter module
 */
struct xmms_sample_converter_St {
	xmms_object_t obj;

	xmms_audio_format_t *from;
	xmms_audio_format_t *to;

	gboolean same;
	gboolean resample;

	/* buffer for result */
	guint bufsiz;
	xmms_sample_t *buf;

	guint interpolator_ratio;
	guint decimator_ratio;

	gfloat incr;
	gfloat offset;

	xmms_sample_t *state;

	xmms_sample_conv_func_t func;

};

static void recalculate_resampler (xmms_sample_converter_t *conv, guint from, guint to);
static xmms_sample_conv_func_t
xmms_sample_conv_get (guint inchannels, xmms_sample_format_t intype,
                      guint outchannels, xmms_sample_format_t outtype,
                      gboolean resample);


/**
 * return a new audioformat.
 */
xmms_audio_format_t *
xmms_sample_audioformat_new (xmms_sample_format_t fmt, guint channels, guint rate)
{
	xmms_audio_format_t *res = g_new0 (xmms_audio_format_t, 1);

	g_return_val_if_fail (res, NULL);

	res->format = fmt;
	res->samplerate = rate;
	res->channels = channels;

	return res;
}

/**
 * Free all resources used by a #xmms_audio_format_t
 */
void
xmms_sample_audioformat_destroy (xmms_audio_format_t *fmt)
{
	g_free (fmt);
}

static void
xmms_sample_converter_destroy (xmms_object_t *obj)
{
	xmms_sample_converter_t *conv = (xmms_sample_converter_t *) obj;

	g_free (conv->buf);
	g_free (conv->state);
}

static xmms_sample_converter_t *
xmms_sample_converter_init (xmms_audio_format_t *from, xmms_audio_format_t *to)
{
	xmms_sample_converter_t *conv = xmms_object_new (xmms_sample_converter_t, xmms_sample_converter_destroy);

	conv->from = from;
	conv->to = to;

	conv->resample = from->samplerate != to->samplerate;
	
	conv->func = xmms_sample_conv_get (from->channels, from->format,
					   to->channels, to->format,
					   conv->resample);

	if (conv->resample)
		recalculate_resampler (conv, from->samplerate, to->samplerate);

	if (from->channels == to->channels &&
	    from->format == to->format &&
	    from->samplerate == to->samplerate) {
		conv->same = TRUE;
	}

	return conv;
}

/**
 * Return the audio format used by the converter as source
 */
xmms_audio_format_t *
xmms_sample_converter_get_from (xmms_sample_converter_t *conv)
{
	g_return_val_if_fail (conv, NULL);

	return conv->from;
}

/**
 * Return the audio format used by the converter as target
 */
xmms_audio_format_t *
xmms_sample_converter_get_to (xmms_sample_converter_t *conv)
{
	g_return_val_if_fail (conv, NULL);

	return conv->to;
}

/**
 * Find the best pair of formats
 */
xmms_sample_converter_t *
xmms_sample_audioformats_coerce (GList *declist, GList *outlist)
{
	xmms_audio_format_t *bestdf = NULL, *bestof = NULL;
	GList *dn, *on;
/*	gint bestscore = GINT_MAX;*/
	gint bestscore = 100000;

	for (dn = declist; dn; dn = g_list_next (dn)) {
		for (on = outlist ; on; on = g_list_next (on)) {
			xmms_audio_format_t *df, *of;
			gint score = 0;

			df = dn->data;
			of = on->data;

			if (of->channels > df->channels) {
				/* we loose no quality, just cputime */
				score += of->channels - df->channels;
			} else if (of->channels < df->channels) {
				/* quality loss! */
				score += 10 * (df->channels - of->channels);
			}


			/* the format enum should be ordered in
			   quality order */
			if (of->format > df->format) {
				/* we loose no quality, just cputime */
				score += of->format - df->format;
			} else if (of->format < df->format) {
				/* quality loss! */
				score += 10 * (df->format - of->format);
			}


			if (of->samplerate > df->samplerate) {
				/* we loose no quality, just cputime */
				score += 2 * of->samplerate / df->samplerate;
			} else if (of->samplerate < df->samplerate) {
				/* quality loss! */
				score += 20 * df->samplerate / of->samplerate;
			}

			/*
			XMMS_DBG ("Conversion from %s-%d-%d to %s-%d-%d score %d",
				  xmms_sample_name_get (df->format),
				  df->channels, df->samplerate,
				  xmms_sample_name_get (of->format),
				  of->channels, of->samplerate,
				  score);
			*/

			if (score < bestscore) {
				bestof = of;
				bestdf = df;
				bestscore = score;
			}
		}
	}

	if (!bestof)
		return NULL;


	XMMS_DBG ("Selected conversion from %s-%d-%d to %s-%d-%d of %d+%d formats",
	          xmms_sample_name_get (bestdf->format),
	          bestdf->channels, bestdf->samplerate,
	          xmms_sample_name_get (bestof->format),
	          bestof->channels, bestof->samplerate,
	          g_list_length (declist), g_list_length (outlist));
	
	return xmms_sample_converter_init (bestdf, bestof);

}

/**
 * convert from milliseconds to samples for this format.
 */
guint
xmms_sample_ms_to_samples (xmms_audio_format_t *f, guint milliseconds)
{
	return (guint)(((gdouble) f->samplerate) * milliseconds / 1000);
}

/**
 * Convert from samples to milliseconds for this format
 */
guint
xmms_sample_samples_to_ms (xmms_audio_format_t *f, guint samples)
{
	return (guint) (((gdouble)samples) * 1000.0 / f->samplerate);
}

/**
 * Convert from bytes to milliseconds for this format
 */
guint
xmms_sample_bytes_to_ms (xmms_audio_format_t *f, guint bytes)
{
	guint samples = bytes / xmms_sample_size_get (f->format) / f->channels;
	return xmms_sample_samples_to_ms (f, samples);
}

static void
recalculate_resampler (xmms_sample_converter_t *conv, guint from, guint to)
{
	guint a,b;

	/* calculate ratio */
	if(from > to){
		a = from;
		b = to;
	} else {
		b = to;
		a = from;
	}

	while (b != 0) { /* good 'ol euclid is helpful as usual */
		guint t = a % b;
		a = b;
		b = t;
	}

	XMMS_DBG ("Resampling ratio: %d:%d", 
		  from / a, to / a);

	conv->interpolator_ratio = to/a;
	conv->decimator_ratio = from/a;

	conv->incr = (gfloat)conv->decimator_ratio / (gfloat)conv->interpolator_ratio;

	conv->state = g_malloc0 (xmms_sample_size_get (conv->from->format) * conv->from->channels);

	/*
	 * calculate filter here
	 *
	 * We don't use no stinkning filter. Maybe we should,
	 * but I'm deaf anyway, I wont hear any difference.
	 */

}

/**
 * do the actual converstion between two audio formats.
 */
void
xmms_sample_convert (xmms_sample_converter_t *conv, xmms_sample_t *in, guint len, xmms_sample_t **out, guint *outlen)
{
	int inusiz, outusiz;
	int olen;
	guint res;

	inusiz = xmms_sample_size_get (conv->from->format) * conv->from->channels;

	if (len % inusiz != 0) {
		XMMS_DBG ("%d %d (%d * %d) ((%d))<------", len, inusiz,
			  xmms_sample_size_get (conv->from->format), conv->from->channels, 
			  conv->from->format);
		g_return_if_fail (len % inusiz == 0);
	}

	if (conv->same) {
		*outlen = len;
		*out = in;
		return;
	}

	len /= inusiz;

	outusiz = xmms_sample_size_get (conv->to->format) * conv->to->channels;

	if (conv->resample) {
		olen = (len * outusiz * conv->interpolator_ratio + outusiz) / conv->decimator_ratio;
	} else {
		olen = len * outusiz;
	}
	if (olen > conv->bufsiz) {
		void *t;
		t = g_realloc (conv->buf, olen);
		g_assert (t); /* XXX */
		conv->buf = t;
	}

	res = conv->func (conv, in, len, conv->buf);

	*outlen = res * outusiz;
	*out = conv->buf;

}


/* Include autogenerated converters */
#include "converter.c"
