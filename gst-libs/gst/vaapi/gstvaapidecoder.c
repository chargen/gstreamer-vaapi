/*
 *  gstvaapidecoder.c - VA decoder abstraction
 *
 *  Copyright (C) 2010-2011 Splitted-Desktop Systems
 *  Copyright (C) 2011-2012 Intel Corporation
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation; either version 2.1
 *  of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free
 *  Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301 USA
 */

/**
 * SECTION:gstvaapidecoder
 * @short_description: VA decoder abstraction
 */

#include "sysdeps.h"
#include "gstvaapicompat.h"
#include "gstvaapidecoder.h"
#include "gstvaapidecoder_priv.h"
#include "gstvaapiutils.h"
#include "gstvaapi_priv.h"

#define DEBUG 1
#include "gstvaapidebug.h"

G_DEFINE_TYPE(GstVaapiDecoder, gst_vaapi_decoder, G_TYPE_OBJECT)

enum {
    PROP_0,

    PROP_DISPLAY,
    PROP_CAPS,

    N_PROPERTIES
};

static GParamSpec *g_properties[N_PROPERTIES] = { NULL, };

static void
parser_state_finalize(GstVaapiParserState *ps)
{
    if (ps->input_adapter) {
        gst_adapter_clear(ps->input_adapter);
        g_object_unref(ps->input_adapter);
        ps->input_adapter = NULL;
    }

    if (ps->output_adapter) {
        gst_adapter_clear(ps->output_adapter);
        g_object_unref(ps->output_adapter);
        ps->output_adapter = NULL;
    }
}

static gboolean
parser_state_init(GstVaapiParserState *ps)
{
    ps->input_adapter = gst_adapter_new();
    if (!ps->input_adapter)
        return FALSE;

    ps->output_adapter = gst_adapter_new();
    if (!ps->output_adapter)
        return FALSE;
    return TRUE;
}

static inline GstVaapiDecoderUnit *
parser_state_get_pending_unit(GstVaapiParserState *ps, GstAdapter *adapter)
{
    GstVaapiDecoderUnit * const unit = ps->pending_unit;

    ps->pending_unit = NULL;
    return unit;
}

static inline void
parser_state_set_pending_unit(GstVaapiParserState *ps,
    GstAdapter *adapter, GstVaapiDecoderUnit *unit)
{
    ps->pending_unit = unit;
}

static void
parser_state_prepare(GstVaapiParserState *ps, GstAdapter *adapter)
{
    /* XXX: check we really have a continuity from the previous call */
    if (ps->current_adapter != adapter)
        goto reset;
    return;

reset:
    ps->current_adapter = adapter;
    ps->input_offset2 = -1;
}

static gboolean
push_buffer(GstVaapiDecoder *decoder, GstBuffer *buffer)
{
    GstVaapiDecoderPrivate * const priv = decoder->priv;

    if (!buffer) {
        buffer = gst_buffer_new();
        if (!buffer)
            return FALSE;
        GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_EOS);
    }

    GST_DEBUG("queue encoded data buffer %p (%d bytes)",
              buffer, GST_BUFFER_SIZE(buffer));

    g_queue_push_tail(priv->buffers, buffer);
    return TRUE;
}

static GstBuffer *
pop_buffer(GstVaapiDecoder *decoder)
{
    GstVaapiDecoderPrivate * const priv = decoder->priv;
    GstBuffer *buffer;

    buffer = g_queue_pop_head(priv->buffers);
    if (!buffer)
        return NULL;

    GST_DEBUG("dequeue buffer %p for decoding (%d bytes)",
              buffer, GST_BUFFER_SIZE(buffer));

    return buffer;
}

static GstVaapiDecoderStatus
do_parse(GstVaapiDecoder *decoder,
    GstVideoCodecFrame *base_frame, GstAdapter *adapter, gboolean at_eos,
    guint *got_unit_size_ptr, gboolean *got_frame_ptr)
{
    GstVaapiParserState * const ps = &decoder->priv->parser_state;
    GstVaapiDecoderFrame *frame;
    GstVaapiDecoderUnit *unit;
    GstVaapiDecoderStatus status;

    *got_unit_size_ptr = 0;
    *got_frame_ptr = FALSE;

    frame = gst_video_codec_frame_get_user_data(base_frame);
    if (!frame) {
        frame = gst_vaapi_decoder_frame_new();
        if (!frame)
            return GST_VAAPI_DECODER_STATUS_ERROR_ALLOCATION_FAILED;
        gst_video_codec_frame_set_user_data(base_frame,
            frame, (GDestroyNotify)gst_vaapi_mini_object_unref);
    }

    parser_state_prepare(ps, adapter);

    unit = parser_state_get_pending_unit(ps, adapter);
    if (unit)
        goto got_unit;

    ps->current_frame = base_frame;
    status = GST_VAAPI_DECODER_GET_CLASS(decoder)->parse(decoder,
        adapter, at_eos, &unit);
    if (status != GST_VAAPI_DECODER_STATUS_SUCCESS) {
        if (unit)
            gst_vaapi_decoder_unit_unref(unit);
        return status;
    }

    if (GST_VAAPI_DECODER_UNIT_IS_FRAME_START(unit) && frame->prev_slice) {
        parser_state_set_pending_unit(ps, adapter, unit);
        goto got_frame;
    }

got_unit:
    unit->offset = frame->output_offset;
    frame->units = g_slist_prepend(frame->units, unit);
    frame->output_offset += unit->size;
    if (GST_VAAPI_DECODER_UNIT_IS_SLICE(unit))
        frame->prev_slice = unit;

    *got_unit_size_ptr = unit->size;
    if (GST_VAAPI_DECODER_UNIT_IS_FRAME_END(unit)) {
    got_frame:
        frame->units = g_slist_reverse(frame->units);
        *got_frame_ptr = TRUE;
    }
    return GST_VAAPI_DECODER_STATUS_SUCCESS;
}

static GstVaapiDecoderStatus
do_decode(GstVaapiDecoder *decoder, GstVideoCodecFrame *base_frame)
{
    GstVaapiDecoderClass * const klass = GST_VAAPI_DECODER_GET_CLASS(decoder);
    GstVaapiParserState * const ps = &decoder->priv->parser_state;
    GstVaapiDecoderFrame * const frame = base_frame->user_data;
    GstVaapiDecoderStatus status;
    GSList *l;

    ps->current_frame = base_frame;

    if (klass->start_frame) {
        for (l = frame->units; l != NULL; l = l->next) {
            GstVaapiDecoderUnit * const unit = l->data;
            if (GST_VAAPI_DECODER_UNIT_IS_SLICE(unit)) {
                status = klass->start_frame(decoder, unit);
                if (status != GST_VAAPI_DECODER_STATUS_SUCCESS)
                    return status;
                break;
            }
        }
    }

    for (l = frame->units; l != NULL; l = l->next) {
        GstVaapiDecoderUnit * const unit = l->data;
        if (GST_VAAPI_DECODER_UNIT_IS_SKIPPED(unit))
            continue;
        status = klass->decode(decoder, unit);
        if (status != GST_VAAPI_DECODER_STATUS_SUCCESS)
            return status;
    }

    if (klass->end_frame) {
        status = klass->end_frame(decoder);
        if (status != GST_VAAPI_DECODER_STATUS_SUCCESS)
            return status;
    }
    return GST_VAAPI_DECODER_STATUS_SUCCESS;
}

static GstVaapiDecoderStatus
decode_step(GstVaapiDecoder *decoder)
{
    GstVaapiDecoderPrivate * const priv = decoder->priv;
    GstVaapiParserState * const ps = &priv->parser_state;
    GstVaapiDecoderStatus status;
    GstBuffer *buffer;
    gboolean at_eos, got_frame;
    guint got_unit_size;

    status = gst_vaapi_decoder_check_status(decoder);
    if (status != GST_VAAPI_DECODER_STATUS_SUCCESS)
        return status;

    do {
        buffer = pop_buffer(decoder);
        if (!buffer)
            return GST_VAAPI_DECODER_STATUS_ERROR_NO_DATA;

        at_eos = GST_BUFFER_IS_EOS(buffer);
        if (!at_eos)
            gst_adapter_push(ps->input_adapter, buffer);

        do {
            if (!ps->current_frame) {
                ps->current_frame = g_slice_new0(GstVideoCodecFrame);
                if (!ps->current_frame)
                    return GST_VAAPI_DECODER_STATUS_ERROR_ALLOCATION_FAILED;
                ps->current_frame->ref_count = 1;
            }

            status = do_parse(decoder, ps->current_frame,
                ps->input_adapter, at_eos, &got_unit_size, &got_frame);
            GST_DEBUG("parse frame (status = %d)", status);
            if (status != GST_VAAPI_DECODER_STATUS_SUCCESS)
                break;

            if (got_unit_size > 0) {
                buffer = gst_adapter_take_buffer(ps->input_adapter,
                    got_unit_size);
                if (gst_adapter_available(ps->output_adapter) == 0) {
                    ps->current_frame->pts =
                        gst_adapter_prev_timestamp(ps->input_adapter, NULL);
                }
                gst_adapter_push(ps->output_adapter, buffer);
            }

            if (got_frame) {
                ps->current_frame->input_buffer = gst_adapter_take_buffer(
                    ps->output_adapter,
                    gst_adapter_available(ps->output_adapter));

                status = do_decode(decoder, ps->current_frame);
                GST_DEBUG("decode frame (status = %d)", status);

                gst_video_codec_frame_unref(ps->current_frame);
                ps->current_frame = NULL;
            }
        } while (status == GST_VAAPI_DECODER_STATUS_SUCCESS &&
                 gst_adapter_available(ps->input_adapter) > 0);
    } while (status == GST_VAAPI_DECODER_STATUS_ERROR_NO_DATA);
    return status;
}

static inline void
push_frame(GstVaapiDecoder *decoder, GstVideoCodecFrame *frame)
{
    GstVaapiDecoderPrivate * const priv = decoder->priv;

    GST_DEBUG("queue decoded surface %" GST_VAAPI_ID_FORMAT,
              GST_VAAPI_ID_ARGS(gst_vaapi_surface_proxy_get_surface_id(
                                    frame->user_data)));

    g_queue_push_tail(priv->frames, frame);
}

static inline GstVideoCodecFrame *
pop_frame(GstVaapiDecoder *decoder)
{
    GstVaapiDecoderPrivate * const priv = decoder->priv;
    GstVideoCodecFrame *frame;

    frame = g_queue_pop_head(priv->frames);
    if (!frame)
        return NULL;

    GST_DEBUG("dequeue decoded surface %" GST_VAAPI_ID_FORMAT,
              GST_VAAPI_ID_ARGS(gst_vaapi_surface_proxy_get_surface_id(
                                    frame->user_data)));

    return frame;
}

static void
set_caps(GstVaapiDecoder *decoder, const GstCaps *caps)
{
    GstVaapiDecoderPrivate * const priv = decoder->priv;
    GstVideoCodecState * const codec_state = priv->codec_state;
    GstStructure * const structure = gst_caps_get_structure(caps, 0);
    GstVaapiProfile profile;
    const GValue *v_codec_data;

    profile = gst_vaapi_profile_from_caps(caps);
    if (!profile)
        return;

    priv->codec = gst_vaapi_profile_get_codec(profile);
    if (!priv->codec)
        return;

    if (!gst_video_info_from_caps(&codec_state->info, caps))
        return;

    codec_state->caps = gst_caps_copy(caps);

    v_codec_data = gst_structure_get_value(structure, "codec_data");
    if (v_codec_data)
        gst_buffer_replace(&codec_state->codec_data,
            gst_value_get_buffer(v_codec_data));
}

static inline GstCaps *
get_caps(GstVaapiDecoder *decoder)
{
    return GST_VAAPI_DECODER_CODEC_STATE(decoder)->caps;
}

static void
clear_queue(GQueue *q, GDestroyNotify destroy)
{
    while (!g_queue_is_empty(q))
        destroy(g_queue_pop_head(q));
}

static void
gst_vaapi_decoder_finalize(GObject *object)
{
    GstVaapiDecoder * const        decoder = GST_VAAPI_DECODER(object);
    GstVaapiDecoderPrivate * const priv    = decoder->priv;

    gst_video_codec_state_unref(priv->codec_state);
    priv->codec_state = NULL;

    parser_state_finalize(&priv->parser_state);
 
    if (priv->buffers) {
        clear_queue(priv->buffers, (GDestroyNotify)gst_buffer_unref);
        g_queue_free(priv->buffers);
        priv->buffers = NULL;
    }

    if (priv->frames) {
        clear_queue(priv->frames, (GDestroyNotify)
            gst_video_codec_frame_unref);
        g_queue_free(priv->frames);
        priv->frames = NULL;
    }

    g_clear_object(&priv->context);
    priv->va_context = VA_INVALID_ID;

    g_clear_object(&priv->display);
    priv->va_display = NULL;

    G_OBJECT_CLASS(gst_vaapi_decoder_parent_class)->finalize(object);
}

static void
gst_vaapi_decoder_set_property(
    GObject      *object,
    guint         prop_id,
    const GValue *value,
    GParamSpec   *pspec
)
{
    GstVaapiDecoder * const        decoder = GST_VAAPI_DECODER(object);
    GstVaapiDecoderPrivate * const priv    = decoder->priv;

    switch (prop_id) {
    case PROP_DISPLAY:
        priv->display = g_object_ref(g_value_get_object(value));
        if (priv->display)
            priv->va_display = gst_vaapi_display_get_display(priv->display);
        else
            priv->va_display = NULL;
        break;
    case PROP_CAPS:
        set_caps(decoder, g_value_get_pointer(value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
gst_vaapi_decoder_get_property(
    GObject    *object,
    guint       prop_id,
    GValue     *value,
    GParamSpec *pspec
)
{
    GstVaapiDecoder * const decoder = GST_VAAPI_DECODER_CAST(object);
    GstVaapiDecoderPrivate * const priv = decoder->priv;

    switch (prop_id) {
    case PROP_DISPLAY:
        g_value_set_object(value, priv->display);
        break;
    case PROP_CAPS:
        gst_value_set_caps(value, get_caps(decoder));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
gst_vaapi_decoder_class_init(GstVaapiDecoderClass *klass)
{
    GObjectClass * const object_class = G_OBJECT_CLASS(klass);

    g_type_class_add_private(klass, sizeof(GstVaapiDecoderPrivate));

    object_class->finalize     = gst_vaapi_decoder_finalize;
    object_class->set_property = gst_vaapi_decoder_set_property;
    object_class->get_property = gst_vaapi_decoder_get_property;

    /**
     * GstVaapiDecoder:display:
     *
     * The #GstVaapiDisplay this decoder is bound to.
     */
    g_properties[PROP_DISPLAY] =
         g_param_spec_object("display",
                             "Display",
                             "The GstVaapiDisplay this decoder is bound to",
                             GST_VAAPI_TYPE_DISPLAY,
                             G_PARAM_READWRITE|G_PARAM_CONSTRUCT_ONLY);

    g_properties[PROP_CAPS] =
         g_param_spec_pointer("caps",
                              "Decoder caps",
                              "The decoder caps",
                              G_PARAM_READWRITE|G_PARAM_CONSTRUCT_ONLY);

    g_object_class_install_properties(object_class, N_PROPERTIES, g_properties);
}

static void
gst_vaapi_decoder_init(GstVaapiDecoder *decoder)
{
    GstVaapiDecoderPrivate *priv = GST_VAAPI_DECODER_GET_PRIVATE(decoder);
    GstVideoCodecState *codec_state;

    parser_state_init(&priv->parser_state);

    codec_state = g_slice_new0(GstVideoCodecState);
    codec_state->ref_count = 1;
    gst_video_info_init(&codec_state->info);

    decoder->priv               = priv;
    priv->display               = NULL;
    priv->va_display            = NULL;
    priv->context               = NULL;
    priv->va_context            = VA_INVALID_ID;
    priv->codec                 = 0;
    priv->codec_state           = codec_state;
    priv->buffers               = g_queue_new();
    priv->frames                = g_queue_new();
}

/**
 * gst_vaapi_decoder_get_codec:
 * @decoder: a #GstVaapiDecoder
 *
 * Retrieves the @decoder codec type.
 *
 * Return value: the #GstVaapiCodec type for @decoder
 */
GstVaapiCodec
gst_vaapi_decoder_get_codec(GstVaapiDecoder *decoder)
{
    g_return_val_if_fail(GST_VAAPI_IS_DECODER(decoder), (GstVaapiCodec)0);

    return decoder->priv->codec;
}

/**
 * gst_vaapi_decoder_get_codec_state:
 * @decoder: a #GstVaapiDecoder
 *
 * Retrieves the @decoder codec state. The caller owns an extra reference
 * to the #GstVideoCodecState, so gst_video_codec_state_unref() shall be
 * called after usage.
 *
 * Return value: the #GstVideoCodecState object for @decoder
 */
GstVideoCodecState *
gst_vaapi_decoder_get_codec_state(GstVaapiDecoder *decoder)
{
    g_return_val_if_fail(GST_VAAPI_IS_DECODER(decoder), NULL);

    return gst_video_codec_state_ref(decoder->priv->codec_state);
}

/**
 * gst_vaapi_decoder_get_caps:
 * @decoder: a #GstVaapiDecoder
 *
 * Retrieves the @decoder caps. The deocder owns the returned caps, so
 * use gst_caps_ref() whenever necessary.
 *
 * Return value: the @decoder caps
 */
GstCaps *
gst_vaapi_decoder_get_caps(GstVaapiDecoder *decoder)
{
    return get_caps(decoder);
}

/**
 * gst_vaapi_decoder_put_buffer:
 * @decoder: a #GstVaapiDecoder
 * @buf: a #GstBuffer
 *
 * Queues a #GstBuffer to the HW decoder. The decoder holds a
 * reference to @buf.
 *
 * Caller can notify an End-Of-Stream with @buf set to %NULL. However,
 * if an empty buffer is passed, i.e. a buffer with %NULL data pointer
 * or size equals to zero, then the function ignores this buffer and
 * returns %TRUE.
 *
 * Return value: %TRUE on success
 */
gboolean
gst_vaapi_decoder_put_buffer(GstVaapiDecoder *decoder, GstBuffer *buf)
{
    g_return_val_if_fail(GST_VAAPI_IS_DECODER(decoder), FALSE);

    if (buf) {
        if (!GST_BUFFER_DATA(buf) || GST_BUFFER_SIZE(buf) <= 0)
            return TRUE;
        buf = gst_buffer_ref(buf);
    }
    return push_buffer(decoder, buf);
}

/**
 * gst_vaapi_decoder_get_surface:
 * @decoder: a #GstVaapiDecoder
 * @out_proxy_ptr: the next decoded surface as a #GstVaapiSurfaceProxy
 *
 * Flushes encoded buffers to the decoder and returns a decoded
 * surface, if any.
 *
 * On successful return, *@out_proxy_ptr contains the decoded surface
 * as a #GstVaapiSurfaceProxy. The caller owns this object, so
 * gst_vaapi_surface_proxy_unref() shall be called after usage.
 *
 * Return value: a #GstVaapiDecoderStatus
 */
GstVaapiDecoderStatus
gst_vaapi_decoder_get_surface(GstVaapiDecoder *decoder,
    GstVaapiSurfaceProxy **out_proxy_ptr)
{
    GstVideoCodecFrame *frame;
    GstVaapiDecoderStatus status;

    g_return_val_if_fail(GST_VAAPI_IS_DECODER(decoder),
        GST_VAAPI_DECODER_STATUS_ERROR_INVALID_PARAMETER);
    g_return_val_if_fail(out_proxy_ptr != NULL,
        GST_VAAPI_DECODER_STATUS_ERROR_INVALID_PARAMETER);

    frame = pop_frame(decoder);
    if (!frame) {
        do {
            status = decode_step(decoder);
        } while (status == GST_VAAPI_DECODER_STATUS_SUCCESS);
        frame = pop_frame(decoder);
    }

    if (frame) {
        *out_proxy_ptr = gst_vaapi_surface_proxy_ref(frame->user_data);
        gst_video_codec_frame_unref(frame);
        status = GST_VAAPI_DECODER_STATUS_SUCCESS;
    }
    else {
        *out_proxy_ptr = NULL;
        if (status == GST_VAAPI_DECODER_STATUS_SUCCESS)
            status = GST_VAAPI_DECODER_STATUS_ERROR_NO_DATA;
    }
    return status;
}

/**
 * gst_vaapi_decoder_get_frame:
 * @decoder: a #GstVaapiDecoder
 * @out_frame_ptr: the next decoded frame as a #GstVideoCodecFrame
 *
 * On successful return, *@out_frame_ptr contains the next decoded
 * frame available as a #GstVideoCodecFrame. The caller owns this
 * object, so gst_video_codec_frame_unref() shall be called after
 * usage. Otherwise, @GST_VAAPI_DECODER_STATUS_ERROR_NO_DATA is
 * returned if no decoded frame is available.
 *
 * The actual surface is available as a #GstVaapiSurfaceProxy attached
 * to the user-data anchor of the output frame. Ownership of the proxy
 * is transferred to the frame.
 *
 * Return value: a #GstVaapiDecoderStatus
 */
GstVaapiDecoderStatus
gst_vaapi_decoder_get_frame(GstVaapiDecoder *decoder,
    GstVideoCodecFrame **out_frame_ptr)
{
    GstVideoCodecFrame *out_frame;

    g_return_val_if_fail(GST_VAAPI_IS_DECODER(decoder),
        GST_VAAPI_DECODER_STATUS_ERROR_INVALID_PARAMETER);
    g_return_val_if_fail(out_frame_ptr != NULL,
        GST_VAAPI_DECODER_STATUS_ERROR_INVALID_PARAMETER);

    out_frame = pop_frame(decoder);
    if (!out_frame)
        return GST_VAAPI_DECODER_STATUS_ERROR_NO_DATA;

    *out_frame_ptr = out_frame;
    return GST_VAAPI_DECODER_STATUS_SUCCESS;
}

void
gst_vaapi_decoder_set_picture_size(
    GstVaapiDecoder    *decoder,
    guint               width,
    guint               height
)
{
    GstVideoCodecState * const codec_state = decoder->priv->codec_state;
    gboolean size_changed = FALSE;

    if (codec_state->info.width != width) {
        GST_DEBUG("picture width changed to %d", width);
        codec_state->info.width = width;
        gst_caps_set_simple(codec_state->caps,
            "width", G_TYPE_INT, width, NULL);
        size_changed = TRUE;
    }

    if (codec_state->info.height != height) {
        GST_DEBUG("picture height changed to %d", height);
        codec_state->info.height = height;
        gst_caps_set_simple(codec_state->caps,
            "height", G_TYPE_INT, height, NULL);
        size_changed = TRUE;
    }

    if (size_changed)
        g_object_notify_by_pspec(G_OBJECT(decoder), g_properties[PROP_CAPS]);
}

void
gst_vaapi_decoder_set_framerate(
    GstVaapiDecoder    *decoder,
    guint               fps_n,
    guint               fps_d
)
{
    GstVideoCodecState * const codec_state = decoder->priv->codec_state;

    if (!fps_n || !fps_d)
        return;

    if (codec_state->info.fps_n != fps_n || codec_state->info.fps_d != fps_d) {
        GST_DEBUG("framerate changed to %u/%u", fps_n, fps_d);
        codec_state->info.fps_n = fps_n;
        codec_state->info.fps_d = fps_d;
        gst_caps_set_simple(codec_state->caps,
            "framerate", GST_TYPE_FRACTION, fps_n, fps_d, NULL);
        g_object_notify_by_pspec(G_OBJECT(decoder), g_properties[PROP_CAPS]);
    }
}

void
gst_vaapi_decoder_set_pixel_aspect_ratio(
    GstVaapiDecoder    *decoder,
    guint               par_n,
    guint               par_d
)
{
    GstVideoCodecState * const codec_state = decoder->priv->codec_state;

    if (!par_n || !par_d)
        return;

    if (codec_state->info.par_n != par_n || codec_state->info.par_d != par_d) {
        GST_DEBUG("pixel-aspect-ratio changed to %u/%u", par_n, par_d);
        codec_state->info.par_n = par_n;
        codec_state->info.par_d = par_d;
        gst_caps_set_simple(codec_state->caps,
            "pixel-aspect-ratio", GST_TYPE_FRACTION, par_n, par_d, NULL);
        g_object_notify_by_pspec(G_OBJECT(decoder), g_properties[PROP_CAPS]);
    }
}

static const gchar *
gst_interlace_mode_to_string(GstVideoInterlaceMode mode)
{
    switch (mode) {
    case GST_VIDEO_INTERLACE_MODE_PROGRESSIVE:  return "progressive";
    case GST_VIDEO_INTERLACE_MODE_INTERLEAVED:  return "interleaved";
    case GST_VIDEO_INTERLACE_MODE_MIXED:        return "mixed";
    }
    return "<unknown>";
}

void
gst_vaapi_decoder_set_interlace_mode(GstVaapiDecoder *decoder,
    GstVideoInterlaceMode mode)
{
    GstVideoCodecState * const codec_state = decoder->priv->codec_state;

    if (codec_state->info.interlace_mode != mode) {
        GST_DEBUG("interlace mode changed to %s",
                  gst_interlace_mode_to_string(mode));
        codec_state->info.interlace_mode = mode;
        gst_caps_set_simple(codec_state->caps, "interlaced",
            G_TYPE_BOOLEAN, mode != GST_VIDEO_INTERLACE_MODE_PROGRESSIVE, NULL);
        g_object_notify_by_pspec(G_OBJECT(decoder), g_properties[PROP_CAPS]);
    }
}

void
gst_vaapi_decoder_set_interlaced(GstVaapiDecoder *decoder, gboolean interlaced)
{
    gst_vaapi_decoder_set_interlace_mode(decoder,
        (interlaced ?
         GST_VIDEO_INTERLACE_MODE_INTERLEAVED :
         GST_VIDEO_INTERLACE_MODE_PROGRESSIVE));
}

gboolean
gst_vaapi_decoder_ensure_context(
    GstVaapiDecoder     *decoder,
    GstVaapiContextInfo *cip
)
{
    GstVaapiDecoderPrivate * const priv = decoder->priv;

    gst_vaapi_decoder_set_picture_size(decoder, cip->width, cip->height);

    if (priv->context) {
        if (!gst_vaapi_context_reset_full(priv->context, cip))
            return FALSE;
    }
    else {
        priv->context = gst_vaapi_context_new_full(priv->display, cip);
        if (!priv->context)
            return FALSE;
    }
    priv->va_context = gst_vaapi_context_get_id(priv->context);
    return TRUE;
}

void
gst_vaapi_decoder_push_frame(GstVaapiDecoder *decoder,
    GstVideoCodecFrame *frame)
{
    push_frame(decoder, frame);
}

GstVaapiDecoderStatus
gst_vaapi_decoder_check_status(GstVaapiDecoder *decoder)
{
    GstVaapiDecoderPrivate * const priv = decoder->priv;

    if (priv->context && gst_vaapi_context_get_surface_count(priv->context) < 1)
        return GST_VAAPI_DECODER_STATUS_ERROR_NO_SURFACE;
    return GST_VAAPI_DECODER_STATUS_SUCCESS;
}

GstVaapiDecoderStatus
gst_vaapi_decoder_parse(GstVaapiDecoder *decoder,
    GstVideoCodecFrame *base_frame, GstAdapter *adapter, gboolean at_eos,
    guint *got_unit_size_ptr, gboolean *got_frame_ptr)
{
    g_return_val_if_fail(GST_VAAPI_IS_DECODER(decoder),
        GST_VAAPI_DECODER_STATUS_ERROR_INVALID_PARAMETER);
    g_return_val_if_fail(base_frame != NULL,
        GST_VAAPI_DECODER_STATUS_ERROR_INVALID_PARAMETER);
    g_return_val_if_fail(adapter != NULL,
        GST_VAAPI_DECODER_STATUS_ERROR_INVALID_PARAMETER);
    g_return_val_if_fail(got_unit_size_ptr != NULL,
        GST_VAAPI_DECODER_STATUS_ERROR_INVALID_PARAMETER);
    g_return_val_if_fail(got_frame_ptr != NULL,
        GST_VAAPI_DECODER_STATUS_ERROR_INVALID_PARAMETER);

    return do_parse(decoder, base_frame, adapter, at_eos,
        got_unit_size_ptr, got_frame_ptr);
}

GstVaapiDecoderStatus
gst_vaapi_decoder_decode(GstVaapiDecoder *decoder, GstVideoCodecFrame *frame)
{
    GstVaapiDecoderStatus status;

    g_return_val_if_fail(GST_VAAPI_IS_DECODER(decoder),
        GST_VAAPI_DECODER_STATUS_ERROR_INVALID_PARAMETER);
    g_return_val_if_fail(frame != NULL,
        GST_VAAPI_DECODER_STATUS_ERROR_INVALID_PARAMETER);
    g_return_val_if_fail(frame->user_data != NULL,
        GST_VAAPI_DECODER_STATUS_ERROR_INVALID_PARAMETER);

    status = gst_vaapi_decoder_check_status(decoder);
    if (status != GST_VAAPI_DECODER_STATUS_SUCCESS)
        return status;
    return do_decode(decoder, frame);
}
