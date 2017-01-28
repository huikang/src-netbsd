/*	$NetBSD: audio.c,v 1.298 2017/01/27 05:05:51 nat Exp $	*/

/*-
 * Copyright (c) 2016 Nathanial Sloss <nathanialsloss@yahoo.com.au>
 * All rights reserved.
 *
 * Copyright (c) 2008 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Andrew Doran.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Copyright (c) 1991-1993 Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the Computer Systems
 *	Engineering Group at Lawrence Berkeley Laboratory.
 * 4. Neither the name of the University nor of the Laboratory may be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * This is a (partially) SunOS-compatible /dev/audio driver for NetBSD.
 *
 * This code tries to do something half-way sensible with
 * half-duplex hardware, such as with the SoundBlaster hardware.  With
 * half-duplex hardware allowing O_RDWR access doesn't really make
 * sense.  However, closing and opening the device to "turn around the
 * line" is relatively expensive and costs a card reset (which can
 * take some time, at least for the SoundBlaster hardware).  Instead
 * we allow O_RDWR access, and provide an ioctl to set the "mode",
 * i.e. playing or recording.
 *
 * If you write to a half-duplex device in record mode, the data is
 * tossed.  If you read from the device in play mode, you get silence
 * filled buffers at the rate at which samples are naturally
 * generated.
 *
 * If you try to set both play and record mode on a half-duplex
 * device, playing takes precedence.
 */

/*
 * Locking: there are two locks.
 *
 * - sc_lock, provided by the underlying driver.  This is an adaptive lock,
 *   returned in the second parameter to hw_if->get_locks().  It is known
 *   as the "thread lock".
 *
 *   It serializes access to state in all places except the 
 *   driver's interrupt service routine.  This lock is taken from process
 *   context (example: access to /dev/audio).  It is also taken from soft
 *   interrupt handlers in this module, primarily to serialize delivery of
 *   wakeups.  This lock may be used/provided by modules external to the
 *   audio subsystem, so take care not to introduce a lock order problem. 
 *   LONG TERM SLEEPS MUST NOT OCCUR WITH THIS LOCK HELD.
 *
 * - sc_intr_lock, provided by the underlying driver.  This may be either a
 *   spinlock (at IPL_SCHED or IPL_VM) or an adaptive lock (IPL_NONE or
 *   IPL_SOFT*), returned in the first parameter to hw_if->get_locks().  It
 *   is known as the "interrupt lock".
 *
 *   It provides atomic access to the device's hardware state, and to audio
 *   channel data that may be accessed by the hardware driver's ISR.
 *   In all places outside the ISR, sc_lock must be held before taking
 *   sc_intr_lock.  This is to ensure that groups of hardware operations are
 *   made atomically.  SLEEPS CANNOT OCCUR WITH THIS LOCK HELD.
 *
 * List of hardware interface methods, and which locks are held when each
 * is called by this module:
 *
 *	METHOD			INTR	THREAD  NOTES
 *	----------------------- ------- -------	-------------------------
 *	open 			x	x
 *	close 			x	x
 *	drain 			x	x
 *	query_encoding		-	x
 *	set_params 		-	x
 *	round_blocksize		-	x
 *	commit_settings		-	x
 *	init_output 		x	x
 *	init_input 		x	x
 *	start_output 		x	x
 *	start_input 		x	x
 *	halt_output 		x	x
 *	halt_input 		x	x
 *	speaker_ctl 		x	x
 *	getdev 			-	x
 *	setfd 			-	x
 *	set_port 		-	x
 *	get_port 		-	x
 *	query_devinfo 		-	x
 *	allocm 			-	-	Called at attach time
 *	freem 			-	-	Called at attach time
 *	round_buffersize 	-	x
 *	mappage 		-	-	Mem. unchanged after attach
 *	get_props 		-	x
 *	trigger_output 		x	x
 *	trigger_input 		x	x
 *	dev_ioctl 		-	x
 *	get_locks 		-	-	Called at attach time
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: audio.c,v 1.298 2017/01/27 05:05:51 nat Exp $");

#include "audio.h"
#if NAUDIO > 0

#include <sys/types.h>
#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/fcntl.h>
#include <sys/vnode.h>
#include <sys/select.h>
#include <sys/poll.h>
#include <sys/kmem.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/syslog.h>
#include <sys/kernel.h>
#include <sys/signalvar.h>
#include <sys/conf.h>
#include <sys/audioio.h>
#include <sys/device.h>
#include <sys/intr.h>
#include <sys/kthread.h>
#include <sys/cpu.h>

#include <dev/audio_if.h>
#include <dev/audiovar.h>
#include <dev/auconv.h>
#include <dev/auvolconv.h>

#include <machine/endian.h>

/* #define AUDIO_DEBUG	1 */
#ifdef AUDIO_DEBUG
#define DPRINTF(x)	if (audiodebug) printf x
#define DPRINTFN(n,x)	if (audiodebug>(n)) printf x
int	audiodebug = AUDIO_DEBUG;
#else
#define DPRINTF(x)
#define DPRINTFN(n,x)
#endif

#define ROUNDSIZE(x)	x &= -16	/* round to nice boundary */
#define SPECIFIED(x)	(x != ~0)
#define SPECIFIED_CH(x)	(x != (u_char)~0)

/* #define AUDIO_PM_IDLE */
#ifdef AUDIO_PM_IDLE
int	audio_idle_timeout = 30;
#endif

int	audio_blk_ms = AUDIO_BLK_MS;

int	audiosetinfo(struct audio_softc *, struct audio_info *, bool, int);
int	audiogetinfo(struct audio_softc *, struct audio_info *, int, int);

int	audio_open(dev_t, struct audio_softc *, int, int, struct lwp *);
int	audio_close(struct audio_softc *, int, int, struct lwp *, int);
int	audio_read(struct audio_softc *, struct uio *, int);
int	audio_write(struct audio_softc *, struct uio *, int);
int	audio_ioctl(dev_t, struct audio_softc *, u_long, void *, int,
		    struct lwp *);
int	audio_poll(struct audio_softc *, int, struct lwp *);
int	audio_kqfilter(struct audio_softc *, struct knote *);
paddr_t	audio_mmap(struct audio_softc *, off_t, int);

int	mixer_open(dev_t, struct audio_softc *, int, int, struct lwp *);
int	mixer_close(struct audio_softc *, int, int, struct lwp *);
int	mixer_ioctl(struct audio_softc *, u_long, void *, int, struct lwp *);
static	void mixer_remove(struct audio_softc *);
static	void mixer_signal(struct audio_softc *);

void	audio_init_record(struct audio_softc *, int);
void	audio_init_play(struct audio_softc *, int);
int	audiostartr(struct audio_softc *, int);
int	audiostartp(struct audio_softc *, int);
void	audio_rint(void *);
void	audio_pint(void *);
void	audio_mix(void *);
void	audio_upmix(void *);
void	audio_play_thread(void *);
void	audio_rec_thread(void *);
void	recswvol_func(struct audio_softc *, struct audio_ringbuffer *, int,
		      size_t);
void	recswvol_func8(struct audio_softc *, struct audio_ringbuffer *, int,
		       size_t);
void	recswvol_func16(struct audio_softc *, struct audio_ringbuffer *, int,
			size_t);
void	recswvol_func32(struct audio_softc *, struct audio_ringbuffer *, int,
			size_t);
void	saturate_func(struct audio_softc *);
void	saturate_func8(struct audio_softc *);
void	saturate_func16(struct audio_softc *);
void	saturate_func32(struct audio_softc *);
void	mix_func(struct audio_softc *, struct audio_ringbuffer *, int);
void	mix_func8(struct audio_softc *, struct audio_ringbuffer *, int);
void	mix_func16(struct audio_softc *, struct audio_ringbuffer *, int);
void	mix_func32(struct audio_softc *, struct audio_ringbuffer *, int);
void	mix_write(void *);
void	mix_read(void *);
int	audio_check_params(struct audio_params *);

void	audio_calc_blksize(struct audio_softc *, int, int);
void	audio_fill_silence(struct audio_params *, uint8_t *, int);
int	audio_silence_copyout(struct audio_softc *, int, struct uio *);

void	audio_init_ringbuffer(struct audio_softc *,
			      struct audio_ringbuffer *, int);
int	audio_initbufs(struct audio_softc *, int);
void	audio_calcwater(struct audio_softc *, int);
int	audio_drain(struct audio_softc *, int);
void	audio_clear(struct audio_softc *, int);
void	audio_clear_intr_unlocked(struct audio_softc *sc, int);
static inline void
	audio_pint_silence(struct audio_softc *, struct audio_ringbuffer *,
			   uint8_t *, int, int);
int	audio_alloc_ring(struct audio_softc *, struct audio_ringbuffer *, int,
			 size_t);
void	audio_free_ring(struct audio_softc *, struct audio_ringbuffer *);
static int audio_setup_pfilters(struct audio_softc *, const audio_params_t *,
				stream_filter_list_t *, int);
static int audio_setup_rfilters(struct audio_softc *, const audio_params_t *,
				stream_filter_list_t *, int);
static void audio_stream_dtor(audio_stream_t *);
static int audio_stream_ctor(audio_stream_t *, const audio_params_t *, int);
static void stream_filter_list_append
	(stream_filter_list_t *, stream_filter_factory_t,
	 const audio_params_t *);
static void stream_filter_list_prepend
	(stream_filter_list_t *, stream_filter_factory_t,
	 const audio_params_t *);
static void stream_filter_list_set
	(stream_filter_list_t *, int, stream_filter_factory_t,
	 const audio_params_t *);
int	audio_set_defaults(struct audio_softc *, u_int, int);
static int audio_sysctl_frequency(SYSCTLFN_PROTO);
static int audio_sysctl_precision(SYSCTLFN_PROTO);
static int audio_sysctl_channels(SYSCTLFN_PROTO);

int	audioprobe(device_t, cfdata_t, void *);
void	audioattach(device_t, device_t, void *);
int	audiodetach(device_t, int);
int	audioactivate(device_t, enum devact);
void	audiochilddet(device_t, device_t);
int	audiorescan(device_t, const char *, const int *);

#ifdef AUDIO_PM_IDLE
static void	audio_idle(void *);
static void	audio_activity(device_t, devactive_t);
#endif

static bool	audio_suspend(device_t dv, const pmf_qual_t *);
static bool	audio_resume(device_t dv, const pmf_qual_t *);
static void	audio_volume_down(device_t);
static void	audio_volume_up(device_t);
static void	audio_volume_toggle(device_t);

static void	audio_mixer_capture(struct audio_softc *);
static void	audio_mixer_restore(struct audio_softc *);

static int	audio_get_props(struct audio_softc *);
static bool	audio_can_playback(struct audio_softc *);
static bool	audio_can_capture(struct audio_softc *);

static void	audio_softintr_rd(void *);
static void	audio_softintr_wr(void *);

static int	audio_enter(dev_t, krw_t, struct audio_softc **);
static void	audio_exit(struct audio_softc *);
static int	audio_waitio(struct audio_softc *, kcondvar_t *);

#define AUDIO_OUTPUT_CLASS 0
#define AUDIO_INPUT_CLASS 1

struct portname {
	const char *name;
	int mask;
};
static const struct portname itable[] = {
	{ AudioNmicrophone,	AUDIO_MICROPHONE },
	{ AudioNline,		AUDIO_LINE_IN },
	{ AudioNcd,		AUDIO_CD },
	{ 0, 0 }
};
static const struct portname otable[] = {
	{ AudioNspeaker,	AUDIO_SPEAKER },
	{ AudioNheadphone,	AUDIO_HEADPHONE },
	{ AudioNline,		AUDIO_LINE_OUT },
	{ 0, 0 }
};
void	au_setup_ports(struct audio_softc *, struct au_mixer_ports *,
			mixer_devinfo_t *, const struct portname *);
int	au_set_gain(struct audio_softc *, struct au_mixer_ports *,
			int, int);
void	au_get_gain(struct audio_softc *, struct au_mixer_ports *,
			u_int *, u_char *);
int	au_set_port(struct audio_softc *, struct au_mixer_ports *,
			u_int);
int	au_get_port(struct audio_softc *, struct au_mixer_ports *);
static int
	audio_get_port(struct audio_softc *, mixer_ctrl_t *);
static int
	audio_set_port(struct audio_softc *, mixer_ctrl_t *);
static int
	audio_query_devinfo(struct audio_softc *, mixer_devinfo_t *);
static int audio_set_params
	(struct audio_softc *, int, int, audio_params_t *, audio_params_t *,
	 stream_filter_list_t *, stream_filter_list_t *, int);
static int
audio_query_encoding(struct audio_softc *, struct audio_encoding *);
static int audio_set_vchan_defaults
	(struct audio_softc *, u_int, const struct audio_format *, int);
static int vchan_autoconfig(struct audio_softc *);
int	au_get_lr_value(struct audio_softc *, mixer_ctrl_t *, int *, int *);
int	au_set_lr_value(struct audio_softc *, mixer_ctrl_t *, int, int);
int	au_portof(struct audio_softc *, char *, int);

typedef struct uio_fetcher {
	stream_fetcher_t base;
	struct uio *uio;
	int usedhigh;
	int last_used;
} uio_fetcher_t;

static void	uio_fetcher_ctor(uio_fetcher_t *, struct uio *, int);
static int	uio_fetcher_fetch_to(struct audio_softc *, stream_fetcher_t *,
				     audio_stream_t *, int);
static int	null_fetcher_fetch_to(struct audio_softc *, stream_fetcher_t *,
				      audio_stream_t *, int);

dev_type_open(audioopen);
dev_type_close(audioclose);
dev_type_read(audioread);
dev_type_write(audiowrite);
dev_type_ioctl(audioioctl);
dev_type_poll(audiopoll);
dev_type_mmap(audiommap);
dev_type_kqfilter(audiokqfilter);

const struct cdevsw audio_cdevsw = {
	.d_open = audioopen,
	.d_close = audioclose,
	.d_read = audioread,
	.d_write = audiowrite,
	.d_ioctl = audioioctl,
	.d_stop = nostop,
	.d_tty = notty,
	.d_poll = audiopoll,
	.d_mmap = audiommap,
	.d_kqfilter = audiokqfilter,
	.d_discard = nodiscard,
	.d_flag = D_MCLOSE | D_MPSAFE
};

/* The default audio mode: 8 kHz mono mu-law */
const struct audio_params audio_default = {
	.sample_rate = 8000,
	.encoding = AUDIO_ENCODING_ULAW,
	.precision = 8,
	.validbits = 8,
	.channels = 1,
};

int auto_config_precision[] = { 32, 24, 16, 8 };
int auto_config_channels[] = { 32, 24, 16, 8, 6, 4, 2, 1};
int auto_config_freq[] = { 48000, 44100, 96000, 192000, 32000,
			   22050, 16000, 11025, 8000, 4000 };

CFATTACH_DECL3_NEW(audio, sizeof(struct audio_softc),
    audioprobe, audioattach, audiodetach, audioactivate, audiorescan,
    audiochilddet, DVF_DETACH_SHUTDOWN);

extern struct cfdriver audio_cd;

int
audioprobe(device_t parent, cfdata_t match, void *aux)
{
	struct audio_attach_args *sa;

	sa = aux;
	DPRINTF(("audioprobe: type=%d sa=%p hw=%p\n",
		 sa->type, sa, sa->hwif));
	return (sa->type == AUDIODEV_TYPE_AUDIO) ? 1 : 0;
}

void
audioattach(device_t parent, device_t self, void *aux)
{
	struct audio_softc *sc;
	struct audio_attach_args *sa;
	struct virtual_channel *vc;
	const struct audio_hw_if *hwp;
	const struct sysctlnode *node;
	void *hdlp;
	int error;
	mixer_devinfo_t mi;
	int iclass, mclass, oclass, rclass, props;
	int record_master_found, record_source_found;
	bool can_capture, can_playback;

	sc = device_private(self);
	sc->dev = self;
	sa = aux;
	hwp = sa->hwif;
	hdlp = sa->hdl;
	sc->sc_opens = 0;
	sc->sc_recopens = 0;
	sc->sc_aivalid = false;
 	sc->sc_ready = true;

 	sc->sc_format[0].mode = AUMODE_PLAY | AUMODE_RECORD;
 	sc->sc_format[0].encoding =
#if BYTE_ORDER == LITTLE_ENDIAN
		 AUDIO_ENCODING_SLINEAR_LE;
#else
		 AUDIO_ENCODING_SLINEAR_BE;
#endif
 	sc->sc_format[0].precision = 16;
 	sc->sc_format[0].validbits = 16;
 	sc->sc_format[0].channels = 2;
 	sc->sc_format[0].channel_mask = AUFMT_STEREO;
 	sc->sc_format[0].frequency_type = 1;
 	sc->sc_format[0].frequency[0] = 44100;

	sc->sc_vchan_params.sample_rate = 44100;
#if BYTE_ORDER == LITTLE_ENDIAN
	sc->sc_vchan_params.encoding = AUDIO_ENCODING_SLINEAR_LE;
#else
	sc->sc_vchan_params.encoding = AUDIO_ENCODING_SLINEAR_BE;
#endif
	sc->sc_vchan_params.precision = 16;
	sc->sc_vchan_params.validbits = 16;
	sc->sc_vchan_params.channels = 2;

	sc->sc_trigger_started = false;
	sc->sc_rec_started = false;
	sc->sc_dying = false;
	sc->sc_vchan[0] = kmem_zalloc(sizeof(struct virtual_channel), KM_SLEEP);
	vc = sc->sc_vchan[0];
	memset(sc->sc_audiopid, -1, sizeof(sc->sc_audiopid));
	memset(sc->sc_despid, -1, sizeof(sc->sc_despid));
	vc->sc_open = 0;
	vc->sc_mode = 0;
	vc->sc_npfilters = 0;
	memset(vc->sc_pfilters, 0,
	    sizeof(vc->sc_pfilters));
	vc->sc_lastinfovalid = false;
	vc->sc_swvol = 255;
	vc->sc_recswvol = 255;
	sc->sc_iffreq = 44100;
	sc->sc_precision = 16;
	sc->sc_channels = 2;

	if (auconv_create_encodings(sc->sc_format, VAUDIO_NFORMATS,
	    &sc->sc_encodings) != 0) {
		aprint_error_dev(self, "couldn't create encodings\n");
		return;
	}

	cv_init(&sc->sc_rchan, "audiord");
	cv_init(&sc->sc_wchan, "audiowr");
	cv_init(&sc->sc_lchan, "audiolk");
	cv_init(&sc->sc_condvar,"play");
	cv_init(&sc->sc_rcondvar,"record");

	if (hwp == 0 || hwp->get_locks == 0) {
		aprint_error(": missing method\n");
		panic("audioattach");
	}

	hwp->get_locks(hdlp, &sc->sc_intr_lock, &sc->sc_lock);

#ifdef DIAGNOSTIC
	if (hwp->query_encoding == 0 ||
	    hwp->set_params == 0 ||
	    (hwp->start_output == 0 && hwp->trigger_output == 0) ||
	    (hwp->start_input == 0 && hwp->trigger_input == 0) ||
	    hwp->halt_output == 0 ||
	    hwp->halt_input == 0 ||
	    hwp->getdev == 0 ||
	    hwp->set_port == 0 ||
	    hwp->get_port == 0 ||
	    hwp->query_devinfo == 0 ||
	    hwp->get_props == 0) {
		aprint_error(": missing method\n");
		sc->hw_if = NULL;
		return;
	}
#endif

	sc->hw_if = hwp;
	sc->hw_hdl = hdlp;
	sc->sc_dev = parent;

	mutex_enter(sc->sc_lock);
	props = audio_get_props(sc);
	mutex_exit(sc->sc_lock);

	if (props & AUDIO_PROP_FULLDUPLEX)
		aprint_normal(": full duplex");
	else
		aprint_normal(": half duplex");

	if (props & AUDIO_PROP_PLAYBACK)
		aprint_normal(", playback");
	if (props & AUDIO_PROP_CAPTURE)
		aprint_normal(", capture");
	if (props & AUDIO_PROP_MMAP)
		aprint_normal(", mmap");
	if (props & AUDIO_PROP_INDEPENDENT)
		aprint_normal(", independent");

	aprint_naive("\n");
	aprint_normal("\n");

	mutex_enter(sc->sc_lock);
	can_playback = audio_can_playback(sc);
	can_capture = audio_can_capture(sc);

	if (can_playback) {
		error = audio_alloc_ring(sc, &sc->sc_pr,
	    	    AUMODE_PLAY, AU_RING_SIZE);
		if (error)
			goto bad_play;

		error = audio_alloc_ring(sc, &sc->sc_vchan[0]->sc_mpr,
	    	    AUMODE_PLAY, AU_RING_SIZE);
bad_play:
		if (error) {
			if (sc->sc_pr.s.start != NULL)
				audio_free_ring(sc, &sc->sc_pr);
			sc->hw_if = NULL;
			aprint_error_dev(sc->sc_dev, "could not allocate play "
			    "buffer\n");
			return;
		}
	}
	if (can_capture) {
		error = audio_alloc_ring(sc, &sc->sc_rr,
		    AUMODE_RECORD, AU_RING_SIZE);
		if (error)
			goto bad_rec;

		error = audio_alloc_ring(sc, &sc->sc_vchan[0]->sc_mrr,
		    AUMODE_RECORD, AU_RING_SIZE);
bad_rec:
		if (error) {
			if (sc->sc_vchan[0]->sc_mrr.s.start != NULL)
				audio_free_ring(sc, &sc->sc_vchan[0]->sc_mrr);
			if (sc->sc_pr.s.start != NULL)
				audio_free_ring(sc, &sc->sc_pr);
			if (sc->sc_vchan[0]->sc_mpr.s.start != 0)
				audio_free_ring(sc, &sc->sc_vchan[0]->sc_mpr);
			sc->hw_if = NULL;
			aprint_error_dev(sc->sc_dev, "could not allocate record"
			   " buffer\n");
			return;
		}
	}

	sc->sc_lastgain = 128;
	sc->sc_saturate = true;
	mutex_exit(sc->sc_lock);

	error = vchan_autoconfig(sc);
	if (error != 0) {
		aprint_error_dev(sc->sc_dev, "%s: audio_set_vchan_defaults() "
		    "failed\n", __func__);
	}

	sc->sc_pr.blksize = sc->sc_vchan[0]->sc_mpr.blksize;
	sc->sc_rr.blksize = sc->sc_vchan[0]->sc_mrr.blksize;
	sc->sc_sih_rd = softint_establish(SOFTINT_SERIAL | SOFTINT_MPSAFE,
	    audio_softintr_rd, sc);
	sc->sc_sih_wr = softint_establish(SOFTINT_SERIAL | SOFTINT_MPSAFE,
	    audio_softintr_wr, sc);

	iclass = mclass = oclass = rclass = -1;
	sc->sc_inports.index = -1;
	sc->sc_inports.master = -1;
	sc->sc_inports.nports = 0;
	sc->sc_inports.isenum = false;
	sc->sc_inports.allports = 0;
	sc->sc_inports.isdual = false;
	sc->sc_inports.mixerout = -1;
	sc->sc_inports.cur_port = -1;
	sc->sc_outports.index = -1;
	sc->sc_outports.master = -1;
	sc->sc_outports.nports = 0;
	sc->sc_outports.isenum = false;
	sc->sc_outports.allports = 0;
	sc->sc_outports.isdual = false;
	sc->sc_outports.mixerout = -1;
	sc->sc_outports.cur_port = -1;
	sc->sc_monitor_port = -1;
	/*
	 * Read through the underlying driver's list, picking out the class
	 * names from the mixer descriptions. We'll need them to decode the
	 * mixer descriptions on the next pass through the loop.
	 */
	mutex_enter(sc->sc_lock);
	for(mi.index = 0; ; mi.index++) {
		if (audio_query_devinfo(sc, &mi) != 0)
			break;
		 /*
		  * The type of AUDIO_MIXER_CLASS merely introduces a class.
		  * All the other types describe an actual mixer.
		  */
		if (mi.type == AUDIO_MIXER_CLASS) {
			if (strcmp(mi.label.name, AudioCinputs) == 0)
				iclass = mi.mixer_class;
			if (strcmp(mi.label.name, AudioCmonitor) == 0)
				mclass = mi.mixer_class;
			if (strcmp(mi.label.name, AudioCoutputs) == 0)
				oclass = mi.mixer_class;
			if (strcmp(mi.label.name, AudioCrecord) == 0)
				rclass = mi.mixer_class;
		}
	}
	mutex_exit(sc->sc_lock);

	/* Allocate save area.  Ensure non-zero allocation. */
	sc->sc_nmixer_states = mi.index;
	sc->sc_static_nmixer_states = mi.index;
	sc->sc_mixer_state = kmem_zalloc(sizeof(mixer_ctrl_t) *
	    (sc->sc_nmixer_states + (VAUDIOCHANS + 1) * 2), KM_SLEEP);

	/*
	 * This is where we assign each control in the "audio" model, to the
	 * underlying "mixer" control.  We walk through the whole list once,
	 * assigning likely candidates as we come across them.
	 */
	record_master_found = 0;
	record_source_found = 0;
	mutex_enter(sc->sc_lock);
	for(mi.index = 0; ; mi.index++) {
		if (audio_query_devinfo(sc, &mi) != 0)
			break;
		KASSERT(mi.index < sc->sc_nmixer_states);
		if (mi.type == AUDIO_MIXER_CLASS)
			continue;
		if (mi.mixer_class == iclass) {
			/*
			 * AudioCinputs is only a fallback, when we don't
			 * find what we're looking for in AudioCrecord, so
			 * check the flags before accepting one of these.
			 */
			if (strcmp(mi.label.name, AudioNmaster) == 0
			    && record_master_found == 0)
				sc->sc_inports.master = mi.index;
			if (strcmp(mi.label.name, AudioNsource) == 0
			    && record_source_found == 0) {
				if (mi.type == AUDIO_MIXER_ENUM) {
				    int i;
				    for(i = 0; i < mi.un.e.num_mem; i++)
					if (strcmp(mi.un.e.member[i].label.name,
						    AudioNmixerout) == 0)
						sc->sc_inports.mixerout =
						    mi.un.e.member[i].ord;
				}
				au_setup_ports(sc, &sc->sc_inports, &mi,
				    itable);
			}
			if (strcmp(mi.label.name, AudioNdac) == 0 &&
			    sc->sc_outports.master == -1)
				sc->sc_outports.master = mi.index;
		} else if (mi.mixer_class == mclass) {
			if (strcmp(mi.label.name, AudioNmonitor) == 0)
				sc->sc_monitor_port = mi.index;
		} else if (mi.mixer_class == oclass) {
			if (strcmp(mi.label.name, AudioNmaster) == 0)
				sc->sc_outports.master = mi.index;
			if (strcmp(mi.label.name, AudioNselect) == 0)
				au_setup_ports(sc, &sc->sc_outports, &mi,
				    otable);
		} else if (mi.mixer_class == rclass) {
			/*
			 * These are the preferred mixers for the audio record
			 * controls, so set the flags here, but don't check.
			 */
			if (strcmp(mi.label.name, AudioNmaster) == 0) {
				sc->sc_inports.master = mi.index;
				record_master_found = 1;
			}
#if 1	/* Deprecated. Use AudioNmaster. */
			if (strcmp(mi.label.name, AudioNrecord) == 0) {
				sc->sc_inports.master = mi.index;
				record_master_found = 1;
			}
			if (strcmp(mi.label.name, AudioNvolume) == 0) {
				sc->sc_inports.master = mi.index;
				record_master_found = 1;
			}
#endif
			if (strcmp(mi.label.name, AudioNsource) == 0) {
				if (mi.type == AUDIO_MIXER_ENUM) {
				    int i;
				    for(i = 0; i < mi.un.e.num_mem; i++)
					if (strcmp(mi.un.e.member[i].label.name,
						    AudioNmixerout) == 0)
						sc->sc_inports.mixerout =
						    mi.un.e.member[i].ord;
				}
				au_setup_ports(sc, &sc->sc_inports, &mi,
				    itable);
				record_source_found = 1;
			}
		}
	}
	mutex_exit(sc->sc_lock);
	DPRINTF(("audio_attach: inputs ports=0x%x, input master=%d, "
		 "output ports=0x%x, output master=%d\n",
		 sc->sc_inports.allports, sc->sc_inports.master,
		 sc->sc_outports.allports, sc->sc_outports.master));

	/* sysctl set-up for alternate configs */
	sysctl_createv(&sc->sc_log, 0, NULL, &node,
		0,
		CTLTYPE_NODE, device_xname(sc->sc_dev),
		SYSCTL_DESCR("audio format information"),
		NULL, 0,
		NULL, 0,
		CTL_HW,
		CTL_CREATE, CTL_EOL);

	if (node != NULL) {
		sysctl_createv(&sc->sc_log, 0, NULL, NULL,
			CTLFLAG_READWRITE,
			CTLTYPE_INT, "frequency",
			SYSCTL_DESCR("intermediate frequency"),
			audio_sysctl_frequency, 0,
			(void *)sc, 0,
			CTL_HW, node->sysctl_num,
			CTL_CREATE, CTL_EOL);

		sysctl_createv(&sc->sc_log, 0, NULL, NULL,
			CTLFLAG_READWRITE,
			CTLTYPE_INT, "precision",
			SYSCTL_DESCR("intermediate precision"),
			audio_sysctl_precision, 0,
			(void *)sc, 0,
			CTL_HW, node->sysctl_num,
			CTL_CREATE, CTL_EOL);

		sysctl_createv(&sc->sc_log, 0, NULL, NULL,
			CTLFLAG_READWRITE,
			CTLTYPE_INT, "channels",
			SYSCTL_DESCR("intermediate channels"),
			audio_sysctl_channels, 0,
			(void *)sc, 0,
			CTL_HW, node->sysctl_num,
			CTL_CREATE, CTL_EOL);

		sysctl_createv(&sc->sc_log, 0, NULL, NULL,
			CTLFLAG_READWRITE,
			CTLTYPE_BOOL, "saturate",
			SYSCTL_DESCR("saturate to max. volume"),
			NULL, 0,
			&sc->sc_saturate, 0,
			CTL_HW, node->sysctl_num,
			CTL_CREATE, CTL_EOL);
	}

	selinit(&sc->sc_rsel);
	selinit(&sc->sc_wsel);

#ifdef AUDIO_PM_IDLE
	callout_init(&sc->sc_idle_counter, 0);
	callout_setfunc(&sc->sc_idle_counter, audio_idle, self);
#endif

	if (!pmf_device_register(self, audio_suspend, audio_resume))
		aprint_error_dev(self, "couldn't establish power handler\n");
#ifdef AUDIO_PM_IDLE
	if (!device_active_register(self, audio_activity))
		aprint_error_dev(self, "couldn't register activity handler\n");
#endif

	if (!pmf_event_register(self, PMFE_AUDIO_VOLUME_DOWN,
	    audio_volume_down, true))
		aprint_error_dev(self, "couldn't add volume down handler\n");
	if (!pmf_event_register(self, PMFE_AUDIO_VOLUME_UP,
	    audio_volume_up, true))
		aprint_error_dev(self, "couldn't add volume up handler\n");
	if (!pmf_event_register(self, PMFE_AUDIO_VOLUME_TOGGLE,
	    audio_volume_toggle, true))
		aprint_error_dev(self, "couldn't add volume toggle handler\n");

#ifdef AUDIO_PM_IDLE
	callout_schedule(&sc->sc_idle_counter, audio_idle_timeout * hz);
#endif
	kthread_create(PRI_NONE, KTHREAD_MPSAFE | KTHREAD_MUSTJOIN, NULL,
	    audio_rec_thread, sc, &sc->sc_recthread, "audiorec");
	kthread_create(PRI_NONE, KTHREAD_MPSAFE | KTHREAD_MUSTJOIN, NULL,
	    audio_play_thread, sc, &sc->sc_playthread, "audiomix");
	audiorescan(self, "audio", NULL);
}

int
audioactivate(device_t self, enum devact act)
{
	struct audio_softc *sc = device_private(self);

	switch (act) {
	case DVACT_DEACTIVATE:
		mutex_enter(sc->sc_lock);
		sc->sc_dying = true;
		cv_broadcast(&sc->sc_condvar);
		mutex_exit(sc->sc_lock);
		return 0;
	default:
		return EOPNOTSUPP;
	}
}

int
audiodetach(device_t self, int flags)
{
	struct audio_softc *sc;
	int maj, mn, i, n, rc;

	sc = device_private(self);
	DPRINTF(("audio_detach: sc=%p flags=%d\n", sc, flags));

	/* Start draining existing accessors of the device. */
	if ((rc = config_detach_children(self, flags)) != 0)
		return rc;
	mutex_enter(sc->sc_lock);
	sc->sc_dying = true;
	cv_broadcast(&sc->sc_wchan);
	cv_broadcast(&sc->sc_rchan);
	cv_broadcast(&sc->sc_condvar);
	cv_broadcast(&sc->sc_rcondvar);
	mutex_exit(sc->sc_lock);
	kthread_join(sc->sc_playthread);
	kthread_join(sc->sc_recthread);
	mutex_enter(sc->sc_lock);
	cv_destroy(&sc->sc_condvar);
	cv_destroy(&sc->sc_rcondvar);
	mutex_exit(sc->sc_lock);

	/* delete sysctl nodes */
	sysctl_teardown(&sc->sc_log);

	/* locate the major number */
	maj = cdevsw_lookup_major(&audio_cdevsw);

	/*
	 * Nuke the vnodes for any open instances (calls close).
	 * Will wait until any activity on the device nodes has ceased.
	 *
	 * XXXAD NOT YET.
	 *
	 * XXXAD NEED TO PREVENT NEW REFERENCES THROUGH AUDIO_ENTER().
	 */
	mn = device_unit(self);
	vdevgone(maj, mn | SOUND_DEVICE,    mn | SOUND_DEVICE, VCHR);
	vdevgone(maj, mn | AUDIO_DEVICE,    mn | AUDIO_DEVICE, VCHR);
	vdevgone(maj, mn | AUDIOCTL_DEVICE, mn | AUDIOCTL_DEVICE, VCHR);
	vdevgone(maj, mn | MIXER_DEVICE,    mn | MIXER_DEVICE, VCHR);

	pmf_event_deregister(self, PMFE_AUDIO_VOLUME_DOWN,
	    audio_volume_down, true);
	pmf_event_deregister(self, PMFE_AUDIO_VOLUME_UP,
	    audio_volume_up, true);
	pmf_event_deregister(self, PMFE_AUDIO_VOLUME_TOGGLE,
	    audio_volume_toggle, true);

#ifdef AUDIO_PM_IDLE
	callout_halt(&sc->sc_idle_counter, sc->sc_lock);

	device_active_deregister(self, audio_activity);
#endif

	pmf_device_deregister(self);

	/* free resources */
	for (n = 0; n < VAUDIOCHANS; n++) {
		if (n != 0 && sc->sc_audiopid[n].pid == -1)
			continue;
		audio_free_ring(sc, &sc->sc_vchan[n]->sc_mpr);
		audio_free_ring(sc, &sc->sc_vchan[n]->sc_mrr);
	}
	audio_free_ring(sc, &sc->sc_pr);
	audio_free_ring(sc, &sc->sc_rr);
	for (n = 0; n < VAUDIOCHANS; n++) {
		if (n != 0 && sc->sc_audiopid[n].pid == -1)
			continue;
		for (i = 0; i < sc->sc_vchan[n]->sc_npfilters; i++) {
			sc->sc_vchan[n]->sc_pfilters[i]->dtor
			    (sc->sc_vchan[n]->sc_pfilters[i]);
			sc->sc_vchan[n]->sc_pfilters[i] = NULL;
			audio_stream_dtor(&sc->sc_vchan[n]->sc_pstreams[i]);
		}
		sc->sc_vchan[n]->sc_npfilters = 0;

		for (i = 0; i < sc->sc_vchan[n]->sc_nrfilters; i++) {
			sc->sc_vchan[n]->sc_rfilters[i]->dtor
			    (sc->sc_vchan[n]->sc_rfilters[i]);
			sc->sc_vchan[n]->sc_rfilters[i] = NULL;
			audio_stream_dtor(&sc->sc_vchan[n]->sc_rstreams[i]);
		}
		sc->sc_vchan[n]->sc_nrfilters = 0;
	}

	auconv_delete_encodings(sc->sc_encodings);

	if (sc->sc_sih_rd) {
		softint_disestablish(sc->sc_sih_rd);
		sc->sc_sih_rd = NULL;
	}
	if (sc->sc_sih_wr) {
		softint_disestablish(sc->sc_sih_wr);
		sc->sc_sih_wr = NULL;
	}

	kmem_free(sc->sc_vchan[0], sizeof(struct virtual_channel));

#ifdef AUDIO_PM_IDLE
	callout_destroy(&sc->sc_idle_counter);
#endif
	seldestroy(&sc->sc_rsel);
	seldestroy(&sc->sc_wsel);

	cv_destroy(&sc->sc_rchan);
	cv_destroy(&sc->sc_wchan);
	cv_destroy(&sc->sc_lchan);

	return 0;
}

void
audiochilddet(device_t self, device_t child)
{

	/* we hold no child references, so do nothing */
}

static int
audiosearch(device_t parent, cfdata_t cf, const int *locs, void *aux)
{

	if (config_match(parent, cf, aux))
		config_attach_loc(parent, cf, locs, aux, NULL);

	return 0;
}

int
audiorescan(device_t self, const char *ifattr, const int *flags)
{
	struct audio_softc *sc = device_private(self);

	if (!ifattr_match(ifattr, "audio"))
		return 0;

	config_search_loc(audiosearch, sc->dev, "audio", NULL, NULL);

	return 0;
}


int
au_portof(struct audio_softc *sc, char *name, int class)
{
	mixer_devinfo_t mi;

	for (mi.index = 0; audio_query_devinfo(sc, &mi) == 0; mi.index++) {
		if (mi.mixer_class == class && strcmp(mi.label.name, name) == 0)
			return mi.index;
	}
	return -1;
}

void
au_setup_ports(struct audio_softc *sc, struct au_mixer_ports *ports,
	       mixer_devinfo_t *mi, const struct portname *tbl)
{
	int i, j;

	ports->index = mi->index;
	if (mi->type == AUDIO_MIXER_ENUM) {
		ports->isenum = true;
		for(i = 0; tbl[i].name; i++)
		    for(j = 0; j < mi->un.e.num_mem; j++)
			if (strcmp(mi->un.e.member[j].label.name,
						    tbl[i].name) == 0) {
				ports->allports |= tbl[i].mask;
				ports->aumask[ports->nports] = tbl[i].mask;
				ports->misel[ports->nports] =
				    mi->un.e.member[j].ord;
				ports->miport[ports->nports] =
				    au_portof(sc, mi->un.e.member[j].label.name,
				    mi->mixer_class);
				if (ports->mixerout != -1 &&
				    ports->miport[ports->nports] != -1)
					ports->isdual = true;
				++ports->nports;
			}
	} else if (mi->type == AUDIO_MIXER_SET) {
		for(i = 0; tbl[i].name; i++)
		    for(j = 0; j < mi->un.s.num_mem; j++)
			if (strcmp(mi->un.s.member[j].label.name,
						tbl[i].name) == 0) {
				ports->allports |= tbl[i].mask;
				ports->aumask[ports->nports] = tbl[i].mask;
				ports->misel[ports->nports] =
				    mi->un.s.member[j].mask;
				ports->miport[ports->nports] =
				    au_portof(sc, mi->un.s.member[j].label.name,
				    mi->mixer_class);
				++ports->nports;
			}
	}
}

/*
 * Called from hardware driver.  This is where the MI audio driver gets
 * probed/attached to the hardware driver.
 */
device_t
audio_attach_mi(const struct audio_hw_if *ahwp, void *hdlp, device_t dev)
{
	struct audio_attach_args arg;

#ifdef DIAGNOSTIC
	if (ahwp == NULL) {
		aprint_error("audio_attach_mi: NULL\n");
		return 0;
	}
#endif
	arg.type = AUDIODEV_TYPE_AUDIO;
	arg.hwif = ahwp;
	arg.hdl = hdlp;
	return config_found(dev, &arg, audioprint);
}

#ifdef AUDIO_DEBUG
void	audio_printsc(struct audio_softc *);
void	audio_print_params(const char *, struct audio_params *);

void
audio_printsc(struct audio_softc *sc)
{
	int n;

	for (n = 1; n < VAUDIOCHANS; n++) {
		if (sc->sc_audiopid[n].pid == curproc->p_pid)
			break;
	}

	if (n == VAUDIOCHANS)
		return;

	printf("hwhandle %p hw_if %p ", sc->hw_hdl, sc->hw_if);
	printf("open 0x%x mode 0x%x\n", sc->sc_vchan[n]->sc_open,
	    sc->sc_vchan[n]->sc_mode);
	printf("rchan 0x%x wchan 0x%x ", cv_has_waiters(&sc->sc_rchan),
	    cv_has_waiters(&sc->sc_wchan));
	printf("rring used 0x%x pring used=%d\n",
	       audio_stream_get_used(&sc->sc_vchan[n]->sc_mrr.s),
	       audio_stream_get_used(&sc->sc_vchan[n]->sc_mpr.s));
	printf("rbus 0x%x pbus 0x%x ", sc->sc_vchan[n]->sc_rbus,
	    sc->sc_vchan[n]->sc_pbus);
	printf("blksize %d", sc->sc_vchan[n]->sc_mpr.blksize);
	printf("hiwat %d lowat %d\n", sc->sc_vchan[n]->sc_mpr.usedhigh,
	    sc->sc_vchan[n]->sc_mpr.usedlow);
}

void
audio_print_params(const char *s, struct audio_params *p)
{
	printf("%s enc=%u %uch %u/%ubit %uHz\n", s, p->encoding, p->channels,
	       p->validbits, p->precision, p->sample_rate);
}
#endif

int
audio_alloc_ring(struct audio_softc *sc, struct audio_ringbuffer *r,
		 int direction, size_t bufsize)
{
	const struct audio_hw_if *hw;
	void *hdl;

	hw = sc->hw_if;
	hdl = sc->hw_hdl;
	/*
	 * Alloc DMA play and record buffers
	 */
	if (bufsize < AUMINBUF)
		bufsize = AUMINBUF;
	ROUNDSIZE(bufsize);
	if (hw->round_buffersize) {
		bufsize = hw->round_buffersize(hdl, direction, bufsize);
	}
	if (hw->allocm && (r == &sc->sc_vchan[0]->sc_mpr ||
	    r == &sc->sc_vchan[0]->sc_mrr))
		r->s.start = hw->allocm(hdl, direction, bufsize);
	else
		r->s.start = kmem_zalloc(bufsize, KM_SLEEP);
	if (r->s.start == NULL)
		return ENOMEM;
	r->s.bufsize = bufsize;

	return 0;
}

void
audio_free_ring(struct audio_softc *sc, struct audio_ringbuffer *r)
{
	if (r->s.start == NULL)
		return;

	if (sc->hw_if->freem && (r == &sc->sc_vchan[0]->sc_mpr ||
	    r == &sc->sc_vchan[0]->sc_mrr))
		sc->hw_if->freem(sc->hw_hdl, r->s.start, r->s.bufsize);
	else
		kmem_free(r->s.start, r->s.bufsize);
	r->s.start = NULL;
}

static int
audio_setup_pfilters(struct audio_softc *sc, const audio_params_t *pp,
		     stream_filter_list_t *pfilters, int m)
{
	stream_filter_t *pf[AUDIO_MAX_FILTERS], *of[AUDIO_MAX_FILTERS];
	audio_stream_t ps[AUDIO_MAX_FILTERS], os[AUDIO_MAX_FILTERS];
	struct virtual_channel *vc;
	const audio_params_t *from_param;
	audio_params_t *to_param;
	int i, n, onfilters;

	KASSERT(mutex_owned(sc->sc_lock));
	vc = sc->sc_vchan[m];

	/* Construct new filters. */
	memset(pf, 0, sizeof(pf));
	memset(ps, 0, sizeof(ps));
	from_param = pp;
	for (i = 0; i < pfilters->req_size; i++) {
		n = pfilters->req_size - i - 1;
		to_param = &pfilters->filters[n].param;
		audio_check_params(to_param);
		pf[i] = pfilters->filters[n].factory(sc, from_param, to_param);
		if (pf[i] == NULL)
			break;
		if (audio_stream_ctor(&ps[i], from_param, AU_RING_SIZE))
			break;
		if (i > 0)
			pf[i]->set_fetcher(pf[i], &pf[i - 1]->base);
		from_param = to_param;
	}
	if (i < pfilters->req_size) { /* failure */
		DPRINTF(("%s: pfilters failure\n", __func__));
		for (; i >= 0; i--) {
			if (pf[i] != NULL)
				pf[i]->dtor(pf[i]);
			audio_stream_dtor(&ps[i]);
		}
		return EINVAL;
	}

	/* Swap in new filters. */
	mutex_enter(sc->sc_intr_lock);
	memcpy(of, vc->sc_pfilters, sizeof(of));
	memcpy(os, vc->sc_pstreams, sizeof(os));
	onfilters = vc->sc_npfilters;
	memcpy(vc->sc_pfilters, pf, sizeof(pf));
	memcpy(vc->sc_pstreams, ps, sizeof(ps));
	vc->sc_npfilters = pfilters->req_size;
	for (i = 0; i < pfilters->req_size; i++)
		pf[i]->set_inputbuffer(pf[i], &vc->sc_pstreams[i]);

	/* hardware format and the buffer near to userland */
	if (pfilters->req_size <= 0) {
		vc->sc_mpr.s.param = *pp;
		vc->sc_pustream = &vc->sc_mpr.s;
	} else {
		vc->sc_mpr.s.param = pfilters->filters[0].param;
		vc->sc_pustream = &vc->sc_pstreams[0];
	}
	mutex_exit(sc->sc_intr_lock);

	/* Destroy old filters. */
	for (i = 0; i < onfilters; i++) {
		of[i]->dtor(of[i]);
		audio_stream_dtor(&os[i]);
	}

#ifdef AUDIO_DEBUG
	printf("%s: HW-buffer=%p pustream=%p\n",
	       __func__, &vc->sc_mpr.s, vc->sc_pustream);
	for (i = 0; i < pfilters->req_size; i++) {
		char num[100];
		snprintf(num, 100, "[%d]", i);
		audio_print_params(num, &vc->sc_pstreams[i].param);
	}
	audio_print_params("[HW]", &vc->sc_mpr.s.param);
#endif /* AUDIO_DEBUG */

	return 0;
}

static int
audio_setup_rfilters(struct audio_softc *sc, const audio_params_t *rp,
		     stream_filter_list_t *rfilters, int m)
{
	stream_filter_t *rf[AUDIO_MAX_FILTERS], *of[AUDIO_MAX_FILTERS];
	audio_stream_t rs[AUDIO_MAX_FILTERS], os[AUDIO_MAX_FILTERS];
	struct virtual_channel *vc;
	const audio_params_t *to_param;
	audio_params_t *from_param;
	int i, onfilters;

	KASSERT(mutex_owned(sc->sc_lock));
	vc = sc->sc_vchan[m];

	/* Construct new filters. */
	memset(rf, 0, sizeof(rf));
	memset(rs, 0, sizeof(rs));
	for (i = 0; i < rfilters->req_size; i++) {
		from_param = &rfilters->filters[i].param;
		audio_check_params(from_param);
		to_param = i + 1 < rfilters->req_size
			? &rfilters->filters[i + 1].param : rp;
		rf[i] = rfilters->filters[i].factory(sc, from_param, to_param);
		if (rf[i] == NULL)
			break;
		if (audio_stream_ctor(&rs[i], to_param, AU_RING_SIZE))
			break;
		if (i > 0) {
			rf[i]->set_fetcher(rf[i], &rf[i - 1]->base);
		} else {
			/* rf[0] has no previous fetcher because
			 * the audio hardware fills data to the
			 * input buffer. */
			rf[0]->set_inputbuffer(rf[0], &vc->sc_mrr.s);
		}
	}
	if (i < rfilters->req_size) { /* failure */
		DPRINTF(("%s: rfilters failure\n", __func__));
		for (; i >= 0; i--) {
			if (rf[i] != NULL)
				rf[i]->dtor(rf[i]);
			audio_stream_dtor(&rs[i]);
		}
		return EINVAL;
	}

	/* Swap in new filters. */
	mutex_enter(sc->sc_intr_lock);
	memcpy(of, vc->sc_rfilters, sizeof(of));
	memcpy(os, vc->sc_rstreams, sizeof(os));
	onfilters = vc->sc_nrfilters;
	memcpy(vc->sc_rfilters, rf, sizeof(rf));
	memcpy(vc->sc_rstreams, rs, sizeof(rs));
	vc->sc_nrfilters = rfilters->req_size;
	for (i = 1; i < rfilters->req_size; i++)
		rf[i]->set_inputbuffer(rf[i], &vc->sc_rstreams[i - 1]);

	/* hardware format and the buffer near to userland */
	if (rfilters->req_size <= 0) {
		vc->sc_mrr.s.param = *rp;
		vc->sc_rustream = &vc->sc_mrr.s;
	} else {
		vc->sc_mrr.s.param = rfilters->filters[0].param;
		vc->sc_rustream = &vc->sc_rstreams[rfilters->req_size - 1];
	}
	mutex_exit(sc->sc_intr_lock);

#ifdef AUDIO_DEBUG
	printf("%s: HW-buffer=%p pustream=%p\n",
	       __func__, &vc->sc_mrr.s, vc->sc_rustream);
	audio_print_params("[HW]", &vc->sc_mrr.s.param);
	for (i = 0; i < rfilters->req_size; i++) {
		char num[100];
		snprintf(num, 100, "[%d]", i);
		audio_print_params(num, &vc->sc_rstreams[i].param);
	}
#endif /* AUDIO_DEBUG */

	/* Destroy old filters. */
	for (i = 0; i < onfilters; i++) {
		of[i]->dtor(of[i]);
		audio_stream_dtor(&os[i]);
	}

	return 0;
}

static void
audio_stream_dtor(audio_stream_t *stream)
{

	if (stream->start != NULL)
		kmem_free(stream->start, stream->bufsize);
	memset(stream, 0, sizeof(audio_stream_t));
}

static int
audio_stream_ctor(audio_stream_t *stream, const audio_params_t *param, int size)
{
	int frame_size;

	size = min(size, AU_RING_SIZE);
	stream->bufsize = size;
	stream->start = kmem_zalloc(size, KM_SLEEP);
	frame_size = (param->precision + 7) / 8 * param->channels;
	size = (size / frame_size) * frame_size;
	stream->end = stream->start + size;
	stream->inp = stream->start;
	stream->outp = stream->start;
	stream->used = 0;
	stream->param = *param;
	stream->loop = false;
	return 0;
}

static void
stream_filter_list_append(stream_filter_list_t *list,
			  stream_filter_factory_t factory,
			  const audio_params_t *param)
{

	if (list->req_size >= AUDIO_MAX_FILTERS) {
		printf("%s: increase AUDIO_MAX_FILTERS in sys/dev/audio_if.h\n",
		       __func__);
		return;
	}
	list->filters[list->req_size].factory = factory;
	list->filters[list->req_size].param = *param;
	list->req_size++;
}

static void
stream_filter_list_set(stream_filter_list_t *list, int i,
		       stream_filter_factory_t factory,
		       const audio_params_t *param)
{

	if (i < 0 || i >= AUDIO_MAX_FILTERS) {
		printf("%s: invalid index: %d\n", __func__, i);
		return;
	}

	list->filters[i].factory = factory;
	list->filters[i].param = *param;
	if (list->req_size <= i)
		list->req_size = i + 1;
}

static void
stream_filter_list_prepend(stream_filter_list_t *list,
			   stream_filter_factory_t factory,
			   const audio_params_t *param)
{

	if (list->req_size >= AUDIO_MAX_FILTERS) {
		printf("%s: increase AUDIO_MAX_FILTERS in sys/dev/audio_if.h\n",
		       __func__);
		return;
	}
	memmove(&list->filters[1], &list->filters[0],
		sizeof(struct stream_filter_req) * list->req_size);
	list->filters[0].factory = factory;
	list->filters[0].param = *param;
	list->req_size++;
}

/*
 * Look up audio device and acquire locks for device access.
 */
static int
audio_enter(dev_t dev, krw_t rw, struct audio_softc **scp)
{

	struct audio_softc *sc;

	/* First, find the device and take sc_lock. */
	sc = device_lookup_private(&audio_cd, AUDIOUNIT(dev));
	if (sc == NULL || sc->hw_if == NULL)
		return ENXIO;
	mutex_enter(sc->sc_lock);
	if (sc->sc_dying) {
		mutex_exit(sc->sc_lock);
		return EIO;
	}

	*scp = sc;
	return 0;
}

/*
 * Release reference to device acquired with audio_enter().
 */
static void
audio_exit(struct audio_softc *sc)
{
	cv_broadcast(&sc->sc_lchan);
	mutex_exit(sc->sc_lock);
}

/*
 * Wait for I/O to complete, releasing device lock.
 */
static int
audio_waitio(struct audio_softc *sc, kcondvar_t *chan)
{
	int error;

	KASSERT(mutex_owned(sc->sc_lock));
	cv_broadcast(&sc->sc_lchan);

	/* Wait for pending I/O to complete. */
	error = cv_wait_sig(chan, sc->sc_lock);

	return error;
}

int
audioopen(dev_t dev, int flags, int ifmt, struct lwp *l)
{
	struct audio_softc *sc;
	int error;

	if ((error = audio_enter(dev, RW_WRITER, &sc)) != 0)
		return error;
	device_active(sc->dev, DVA_SYSTEM);
	switch (AUDIODEV(dev)) {
	case SOUND_DEVICE:
	case AUDIO_DEVICE:
	case AUDIOCTL_DEVICE:
		error = audio_open(dev, sc, flags, ifmt, l);
		break;
	case MIXER_DEVICE:
		error = mixer_open(dev, sc, flags, ifmt, l);
		break;
	default:
		error = ENXIO;
		break;
	}
	audio_exit(sc);

	return error;
}

int
audioclose(dev_t dev, int flags, int ifmt, struct lwp *l)
{
	struct audio_softc *sc;
	int error, n;

	if ((error = audio_enter(dev, RW_WRITER, &sc)) != 0)
		return error;
	device_active(sc->dev, DVA_SYSTEM);
	switch (AUDIODEV(dev)) {
	case SOUND_DEVICE:
	case AUDIO_DEVICE:
	case AUDIOCTL_DEVICE:
		for (n = 1; n < VAUDIOCHANS; n++) {
			if (sc->sc_audiopid[n].pid == curproc->p_pid)
				break;
		}
		if (n == VAUDIOCHANS) {
			error = EIO;
			break;	
		}
		error = audio_close(sc, flags, ifmt, l, n);
		break;
	case MIXER_DEVICE:
		error = mixer_close(sc, flags, ifmt, l);
		break;
	default:
		error = ENXIO;
		break;
	}
	audio_exit(sc);

	return error;
}

int
audioread(dev_t dev, struct uio *uio, int ioflag)
{
	struct audio_softc *sc;
	int error;

	if ((error = audio_enter(dev, RW_READER, &sc)) != 0)
		return error;
	switch (AUDIODEV(dev)) {
	case SOUND_DEVICE:
	case AUDIO_DEVICE:
		error = audio_read(sc, uio, ioflag);
		break;
	case AUDIOCTL_DEVICE:
	case MIXER_DEVICE:
		error = ENODEV;
		break;
	default:
		error = ENXIO;
		break;
	}
	audio_exit(sc);

	return error;
}

int
audiowrite(dev_t dev, struct uio *uio, int ioflag)
{
	struct audio_softc *sc;
	int error;

	if ((error = audio_enter(dev, RW_READER, &sc)) != 0)
		return error;
	switch (AUDIODEV(dev)) {
	case SOUND_DEVICE:
	case AUDIO_DEVICE:
		error = audio_write(sc, uio, ioflag);
		break;
	case AUDIOCTL_DEVICE:
	case MIXER_DEVICE:
		error = ENODEV;
		break;
	default:
		error = ENXIO;
		break;
	}
	audio_exit(sc);

	return error;
}

int
audioioctl(dev_t dev, u_long cmd, void *addr, int flag, struct lwp *l)
{
	struct audio_softc *sc;
	int error;
	krw_t rw;

	/* Figure out which lock type we need. */
	switch (cmd) {
	case AUDIO_FLUSH:
	case AUDIO_SETINFO:
	case AUDIO_DRAIN:
	case AUDIO_SETFD:
		rw = RW_WRITER;
		break;
	default:
		rw = RW_READER;
		break;
	}

	if ((error = audio_enter(dev, rw, &sc)) != 0)
		return error;
	switch (AUDIODEV(dev)) {
	case SOUND_DEVICE:
	case AUDIO_DEVICE:
	case AUDIOCTL_DEVICE:
		device_active(sc->dev, DVA_SYSTEM);
		if (IOCGROUP(cmd) == IOCGROUP(AUDIO_MIXER_READ))
			error = mixer_ioctl(sc, cmd, addr, flag, l);
		else
			error = audio_ioctl(dev, sc, cmd, addr, flag, l);
		break;
	case MIXER_DEVICE:
		error = mixer_ioctl(sc, cmd, addr, flag, l);
		break;
	default:
		error = ENXIO;
		break;
	}
	audio_exit(sc);

	return error;
}

int
audiopoll(dev_t dev, int events, struct lwp *l)
{
	struct audio_softc *sc;
	int revents;

	/* Don't bother with device level lock here. */
	sc = device_lookup_private(&audio_cd, AUDIOUNIT(dev));
	if (sc == NULL)
		return ENXIO;
	mutex_enter(sc->sc_lock);
	if (sc->sc_dying) {
		mutex_exit(sc->sc_lock);
		return EIO;
	}
	switch (AUDIODEV(dev)) {
	case SOUND_DEVICE:
	case AUDIO_DEVICE:
		revents = audio_poll(sc, events, l);
		break;
	case AUDIOCTL_DEVICE:
	case MIXER_DEVICE:
		revents = 0;
		break;
	default:
		revents = POLLERR;
		break;
	}
	mutex_exit(sc->sc_lock);

	return revents;
}

int
audiokqfilter(dev_t dev, struct knote *kn)
{
	struct audio_softc *sc;
	int rv;

	/* Don't bother with device level lock here. */
	sc = device_lookup_private(&audio_cd, AUDIOUNIT(dev));
	if (sc == NULL)
		return ENXIO;
	mutex_enter(sc->sc_lock);
	if (sc->sc_dying) {
		mutex_exit(sc->sc_lock);
		return EIO;
	}
	switch (AUDIODEV(dev)) {
	case SOUND_DEVICE:
	case AUDIO_DEVICE:
		rv = audio_kqfilter(sc, kn);
		break;
	case AUDIOCTL_DEVICE:
	case MIXER_DEVICE:
		rv = 1;
		break;
	default:
		rv = 1;
	}
	mutex_exit(sc->sc_lock);

	return rv;
}

paddr_t
audiommap(dev_t dev, off_t off, int prot)
{
	struct audio_softc *sc;
	paddr_t error;

	/*
	 * Acquire a reader lock.  audio_mmap() will drop sc_lock
	 * in order to allow the device's mmap routine to sleep.
	 * Although not yet possible, we want to prevent memory
	 * from being allocated or freed out from under us.
	 */
	if ((error = audio_enter(dev, RW_READER, &sc)) != 0)
		return 1;
	device_active(sc->dev, DVA_SYSTEM); /* XXXJDM */
	switch (AUDIODEV(dev)) {
	case SOUND_DEVICE:
	case AUDIO_DEVICE:
		error = audio_mmap(sc, off, prot);
		break;
	case AUDIOCTL_DEVICE:
	case MIXER_DEVICE:
		error = -1;
		break;
	default:
		error = -1;
		break;
	}
	audio_exit(sc);
	return error;
}

/*
 * Audio driver
 */
void
audio_init_ringbuffer(struct audio_softc *sc, struct audio_ringbuffer *rp,
		      int mode)
{
	int nblks;
	int blksize;

	blksize = rp->blksize;
	if (blksize < AUMINBLK)
		blksize = AUMINBLK;
	if (blksize > rp->s.bufsize / AUMINNOBLK)
		blksize = rp->s.bufsize / AUMINNOBLK;
	ROUNDSIZE(blksize);
	DPRINTF(("audio_init_ringbuffer: MI blksize=%d\n", blksize));
	if (sc->hw_if->round_blocksize)
		blksize = sc->hw_if->round_blocksize(sc->hw_hdl, blksize,
						     mode, &rp->s.param);
	if (blksize <= 0)
		panic("audio_init_ringbuffer: blksize=%d", blksize);
	nblks = rp->s.bufsize / blksize;

	DPRINTF(("audio_init_ringbuffer: final blksize=%d\n", blksize));
	rp->blksize = blksize;
	rp->maxblks = nblks;
	rp->s.end = rp->s.start + nblks * blksize;
	rp->s.outp = rp->s.inp = rp->s.start;
	rp->s.used = 0;
	rp->stamp = 0;
	rp->stamp_last = 0;
	rp->fstamp = 0;
	rp->drops = 0;
	rp->copying = false;
	rp->needfill = false;
	rp->mmapped = false;
	memset(rp->s.start, 0, AU_RING_SIZE);
}

int
audio_initbufs(struct audio_softc *sc, int n)
{
	const struct audio_hw_if *hw;
	struct virtual_channel *vc;
	int error;

	DPRINTF(("audio_initbufs: mode=0x%x\n", sc->sc_vchan[n]->sc_mode));
	vc = sc->sc_vchan[0];
	hw = sc->hw_if;
	if (audio_can_capture(sc) || (sc->sc_vchan[n]->sc_open & AUOPEN_READ)) {
		audio_init_ringbuffer(sc, &sc->sc_vchan[n]->sc_mrr,
		    AUMODE_RECORD);
		if (sc->sc_opens == 0 && hw->init_input &&
		    (sc->sc_vchan[n]->sc_mode & AUMODE_RECORD)) {
			error = hw->init_input(sc->hw_hdl, vc->sc_mrr.s.start,
				       vc->sc_mrr.s.end - vc->sc_mrr.s.start);
			if (error)
				return error;
		}
	}

	if (audio_can_playback(sc) ||
				 (sc->sc_vchan[n]->sc_open & AUOPEN_WRITE)) {
		audio_init_ringbuffer(sc, &sc->sc_vchan[n]->sc_mpr,
		    AUMODE_PLAY);
		sc->sc_vchan[n]->sc_sil_count = 0;
		if (sc->sc_opens == 0 && hw->init_output &&
		    (sc->sc_vchan[n]->sc_mode & AUMODE_PLAY)) {
			error = hw->init_output(sc->hw_hdl, vc->sc_mpr.s.start,
					vc->sc_mpr.s.end - vc->sc_mpr.s.start);
			if (error)
				return error;
		}
	}

#ifdef AUDIO_INTR_TIME
#define double u_long
	if (audio_can_playback(sc)) {
		sc->sc_pnintr = 0;
		sc->sc_pblktime = (u_long)(
		    (double)sc->sc_vchan[n]->sc_mpr.blksize * 100000 /
		    (double)(sc->sc_vchan[n]->sc_pparams.precision / NBBY *
			     sc->sc_vchan[n].sc_pparams.channels *
			     sc->sc_vchan[n].sc_pparams.sample_rate)) * 10;
		DPRINTF(("audio: play blktime = %lu for %d\n",
			 sc->sc_pblktime, sc->sc_vchan[n].sc_mpr.blksize));
	}
	if (audio_can_capture(sc)) {
		sc->sc_rnintr = 0;
		sc->sc_rblktime = (u_long)(
		    (double)sc->sc_vchan[n]->sc_mrr.blksize * 100000 /
		    (double)(sc->sc_vchan[n]->sc_rparams.precision / NBBY *
			     sc->sc_vchan[n]->sc_rparams.channels *
			     sc->sc_vchan[n]->sc_rparams.sample_rate)) * 10;
		DPRINTF(("audio: record blktime = %lu for %d\n",
			 sc->sc_rblktime, sc->sc_vchan[n]->sc_mrr.blksize));
	}
#undef double
#endif

	return 0;
}

void
audio_calcwater(struct audio_softc *sc, int n)
{
	struct virtual_channel *vc = sc->sc_vchan[n];

	/* set high at 100% */
	if (audio_can_playback(sc) && vc && vc->sc_pustream) {
		vc->sc_mpr.usedhigh =
		    vc->sc_pustream->end - vc->sc_pustream->start;
		/* set low at 75% of usedhigh */
		vc->sc_mpr.usedlow = vc->sc_mpr.usedhigh * 3 / 4;
		if (vc->sc_mpr.usedlow == vc->sc_mpr.usedhigh)
			vc->sc_mpr.usedlow -= vc->sc_mpr.blksize;
	}

	if (audio_can_capture(sc) && vc && vc->sc_rustream) {
		vc->sc_mrr.usedhigh =
		    vc->sc_rustream->end - vc->sc_rustream->start -
		    vc->sc_mrr.blksize;
		vc->sc_mrr.usedlow = 0;
		DPRINTF(("%s: plow=%d phigh=%d rlow=%d rhigh=%d\n", __func__,
			 vc->sc_mpr.usedlow, vc->sc_mpr.usedhigh,
			 vc->sc_mrr.usedlow, vc->sc_mrr.usedhigh));
	}
}

int
audio_open(dev_t dev, struct audio_softc *sc, int flags, int ifmt,
    struct lwp *l)
{
	int error, i, n;
	u_int mode;
	const struct audio_hw_if *hw;
	struct virtual_channel *vc;

	KASSERT(mutex_owned(sc->sc_lock));

	if (sc->sc_ready == false || sc->sc_opens >= VAUDIOCHANS)
		return ENXIO;

	for (n = 1; n < VAUDIOCHANS; n++) {
		if (sc->sc_audiopid[n].pid == curproc->p_pid)
			break;
	}
	if (n < VAUDIOCHANS)
		return ENXIO;

	for (n = 1; n < VAUDIOCHANS; n++) {
		if (sc->sc_audiopid[n].pid == -1)
			break;
	}
	if (n == VAUDIOCHANS)
		return ENXIO;

	hw = sc->hw_if;
	if (hw == NULL)
		return ENXIO;

	sc->sc_vchan[n] = kmem_zalloc(sizeof(struct virtual_channel), KM_SLEEP);
	vc = sc->sc_vchan[n];

	vc->sc_open = 0;
	vc->sc_mode = 0;
	vc->sc_sil_count = 0;
	vc->sc_nrfilters = 0;
	memset(vc->sc_rfilters, 0,
	    sizeof(vc->sc_rfilters));
	vc->sc_rbus = false;
	vc->sc_npfilters = 0;
	memset(vc->sc_pfilters, 0,
	    sizeof(vc->sc_pfilters));
	vc->sc_draining = false;
	vc->sc_pbus = false;
	vc->sc_blkset = false;
	vc->sc_lastinfovalid = false;
	vc->sc_swvol = 255;
	vc->sc_recswvol = 255;

	DPRINTF(("audio_open: flags=0x%x sc=%p hdl=%p\n",
		 flags, sc, sc->hw_hdl));

	if (((flags & FREAD) && (vc->sc_open & AUOPEN_READ)) ||
	    ((flags & FWRITE) && (vc->sc_open & AUOPEN_WRITE))) {
		kmem_free(vc, sizeof(struct virtual_channel));
		return EBUSY;
	}

	error = audio_alloc_ring(sc, &vc->sc_mpr,
	    	    AUMODE_PLAY, AU_RING_SIZE);
	if (!error) {
		error = audio_alloc_ring(sc, &vc->sc_mrr,
	    	    AUMODE_RECORD, AU_RING_SIZE);
	}
	if (error) {
		kmem_free(vc, sizeof(struct virtual_channel));
		return error;
	}

	if (sc->sc_opens == 0) {
		if (hw->open != NULL) {
			mutex_enter(sc->sc_intr_lock);
			error = hw->open(sc->hw_hdl, flags);
			mutex_exit(sc->sc_intr_lock);
			if (error) {
				kmem_free(vc,
				    sizeof(struct virtual_channel));
				return error;
			}
		}
		audio_init_ringbuffer(sc, &sc->sc_pr, AUMODE_PLAY);
		audio_init_ringbuffer(sc, &sc->sc_rr, AUMODE_RECORD);
		audio_initbufs(sc, 0);
		sc->schedule_wih = false;
		sc->schedule_rih = false;
		sc->sc_eof = 0;
		vc->sc_rbus = false;
		sc->sc_async_audio = 0;
	}

	mutex_enter(sc->sc_intr_lock);
	vc->sc_full_duplex = 
		(flags & (FWRITE|FREAD)) == (FWRITE|FREAD) &&
		(audio_get_props(sc) & AUDIO_PROP_FULLDUPLEX);
	mutex_exit(sc->sc_intr_lock);

	mode = 0;
	if (flags & FREAD) {
		vc->sc_open |= AUOPEN_READ;
		mode |= AUMODE_RECORD;
	}
	if (flags & FWRITE) {
		vc->sc_open |= AUOPEN_WRITE;
		mode |= AUMODE_PLAY | AUMODE_PLAY_ALL;
	}

	/*
	 * Multiplex device: /dev/audio (MU-Law) and /dev/sound (linear)
	 * The /dev/audio is always (re)set to 8-bit MU-Law mono
	 * For the other devices, you get what they were last set to.
	 */
	error = audio_set_defaults(sc, mode, n);
	if (!error && ISDEVSOUND(dev) && sc->sc_aivalid == true) {
		sc->sc_ai.mode = mode;
		error = audiosetinfo(sc, &sc->sc_ai, true, n);
	}
	if (error)
		goto bad;

#ifdef DIAGNOSTIC
	/*
	 * Sample rate and precision are supposed to be set to proper
	 * default values by the hardware driver, so that it may give
	 * us these values.
	 */
	if (vc->sc_rparams.precision == 0 || vc->sc_pparams.precision == 0) {
		printf("audio_open: 0 precision\n");
		goto bad;
	}
#endif

	/* audio_close() decreases sc_mpr[n].usedlow, recalculate here */
	audio_calcwater(sc, n);

	DPRINTF(("audio_open: done sc_mode = 0x%x\n", vc->sc_mode));

	mutex_enter(sc->sc_intr_lock);
	if (flags & FREAD)
		sc->sc_recopens++;
	sc->sc_opens++;
	sc->sc_audiopid[n].pid = curproc->p_pid;
	sc->sc_despid[n].pid = curproc->p_pid;
	sc->sc_nmixer_states += 2;
	mutex_exit(sc->sc_intr_lock);

	return 0;

bad:
	for (i = 0; i < vc->sc_npfilters; i++) {
		vc->sc_pfilters[i]->dtor(vc->sc_pfilters[i]);
		vc->sc_pfilters[i] = NULL;
		audio_stream_dtor(&vc->sc_pstreams[i]);
	}
	vc->sc_npfilters = 0;
	for (i = 0; i < vc->sc_nrfilters; i++) {
		vc->sc_rfilters[i]->dtor(vc->sc_rfilters[i]);
		vc->sc_rfilters[i] = NULL;
		audio_stream_dtor(&vc->sc_rstreams[i]);
	}
	vc->sc_nrfilters = 0;
	if (hw->close != NULL && sc->sc_opens == 0)
		hw->close(sc->hw_hdl);
	mutex_exit(sc->sc_lock);
	audio_free_ring(sc, &vc->sc_mpr);
	audio_free_ring(sc, &vc->sc_mrr);
	mutex_enter(sc->sc_lock);
	kmem_free(sc->sc_vchan[n], sizeof(struct virtual_channel));
	return error;
}

/*
 * Must be called from task context.
 */
void
audio_init_record(struct audio_softc *sc, int n)
{

	KASSERT(mutex_owned(sc->sc_lock));

	struct virtual_channel *vc = sc->sc_vchan[n];
 
	if (sc->sc_opens != 0)
		return;

	mutex_enter(sc->sc_intr_lock);
	if (sc->hw_if->speaker_ctl &&
	    (!vc->sc_full_duplex || (vc->sc_mode & AUMODE_PLAY) == 0))
		sc->hw_if->speaker_ctl(sc->hw_hdl, SPKR_OFF);
	mutex_exit(sc->sc_intr_lock);
}

/*
 * Must be called from task context.
 */
void
audio_init_play(struct audio_softc *sc, int n)
{

	KASSERT(mutex_owned(sc->sc_lock));
	
	if (sc->sc_opens != 0)
		return;

	mutex_enter(sc->sc_intr_lock);
	sc->sc_vchan[n]->sc_wstamp = sc->sc_vchan[n]->sc_mpr.stamp;
	if (sc->hw_if->speaker_ctl)
		sc->hw_if->speaker_ctl(sc->hw_hdl, SPKR_ON);
	mutex_exit(sc->sc_intr_lock);
}

int
audio_drain(struct audio_softc *sc, int n)
{
	struct audio_ringbuffer *cb;
	int error, drops;
	int cc, i, used;

	KASSERT(mutex_owned(sc->sc_lock));
	KASSERT(mutex_owned(sc->sc_intr_lock));
	
	error = 0;
	DPRINTF(("audio_drain: enter busy=%d\n", sc->sc_vchan[n]->sc_pbus));
	cb = &sc->sc_vchan[n]->sc_mpr;
	if (cb->mmapped)
		return 0;

	used = audio_stream_get_used(&cb->s);
	for (i = 0; i < sc->sc_vchan[n]->sc_npfilters; i++)
		used += audio_stream_get_used(&sc->sc_vchan[n]->sc_pstreams[i]);
	if (used <= 0)
		return 0;

	if (n != 0 && !sc->sc_vchan[n]->sc_pbus) {
		/* We've never started playing, probably because the
		 * block was too short.  Pad it and start now.
		 */
		uint8_t *inp = cb->s.inp;

		cc = cb->blksize - (inp - cb->s.start) % cb->blksize;
		audio_fill_silence(&cb->s.param, inp, cc);
		cb->s.inp = audio_stream_add_inp(&cb->s, inp, cc);
		error = audiostartp(sc, n);
		if (error)
			return error;
	} else if (n == 0)
		goto silence_buffer;
	/*
	 * Play until a silence block has been played, then we
	 * know all has been drained.
	 * XXX This should be done some other way to avoid
	 * playing silence.
	 */
#ifdef DIAGNOSTIC
	if (cb->copying) {
		DPRINTF(("audio_drain: copying in progress!?!\n"));
		cb->copying = false;
	}
#endif
	sc->sc_vchan[n]->sc_draining = true;
	drops = cb->drops;
	error = 0;
	while (cb->drops == drops && !error) {
		DPRINTF(("audio_drain: n=%d, used=%d, drops=%ld\n", n,
			audio_stream_get_used(&sc->sc_vchan[n]->sc_mpr.s),
			cb->drops));
		mutex_exit(sc->sc_intr_lock);
		error = audio_waitio(sc, &sc->sc_wchan);
		mutex_enter(sc->sc_intr_lock);
		if (sc->sc_dying)
			error = EIO;
	}

silence_buffer:

	used = 0;
	if (sc->sc_opens == 1) {
		cb = &sc->sc_pr;
		cc = cb->blksize;
		
		while (used < cb->s.bufsize) {
			memset(sc->sc_pr.s.start, 0, cc);
			mix_write(sc);
			used += cc;
		}
	}
	sc->sc_vchan[n]->sc_draining = false;
	return error;
}

/*
 * Close an audio chip.
 */
/* ARGSUSED */
int
audio_close(struct audio_softc *sc, int flags, int ifmt,
    struct lwp *l, int n)
{
	struct virtual_channel *vc;
	const struct audio_hw_if *hw;
	int o;

	KASSERT(mutex_owned(sc->sc_lock));
	
	if (sc->sc_opens == 0 || n == 0)
		return ENXIO;

	vc = sc->sc_vchan[n];

	hw = sc->hw_if;
	if (hw == NULL)
		return ENXIO;
	mutex_enter(sc->sc_intr_lock);
	DPRINTF(("audio_close: sc=%p\n", sc));
	/* Stop recording. */
	if (sc->sc_recopens == 1 && (flags & FREAD) && vc->sc_rbus) {
		/*
		 * XXX Some drivers (e.g. SB) use the same routine
		 * to halt input and output so don't halt input if
		 * in full duplex mode.  These drivers should be fixed.
		 */
		if (!vc->sc_full_duplex || hw->halt_input != hw->halt_output)
			hw->halt_input(sc->hw_hdl);
		vc->sc_rbus = false;
	}
	/*
	 * Block until output drains, but allow ^C interrupt.
	 */
	vc->sc_mpr.usedlow = vc->sc_mpr.blksize;  /* avoid excessive wakeups */
	/*
	 * If there is pending output, let it drain (unless
	 * the output is paused).
	 */
	if ((flags & FWRITE) && vc->sc_pbus) {
		if (!vc->sc_mpr.pause)
			audio_drain(sc, n);
		vc->sc_pbus = false;
	}
	if (sc->sc_opens == 1) {
		audio_drain(sc, 0);
		if (hw->drain)
			(void)hw->drain(sc->hw_hdl);
		hw->halt_output(sc->hw_hdl);
		sc->sc_trigger_started = false;
	}
	if ((flags & FREAD) && (sc->sc_recopens == 1))
		sc->sc_rec_started = false;

	if (sc->sc_opens == 1 && hw->close != NULL)
		hw->close(sc->hw_hdl);
	if (sc->sc_opens == 1)
		sc->sc_async_audio = 0;

	vc->sc_open = 0;
	vc->sc_mode = 0;
	vc->sc_full_duplex = 0;
	
	for (o = 0; o < vc->sc_npfilters; o++) {
		vc->sc_pfilters[o]->dtor(vc->sc_pfilters[o]);
		vc->sc_pfilters[o] = NULL;
		audio_stream_dtor(&vc->sc_pstreams[o]);
	}
	vc->sc_npfilters = 0;
	for (o = 0; o < vc->sc_nrfilters; o++) {
		vc->sc_rfilters[o]->dtor(vc->sc_rfilters[o]);
		vc->sc_rfilters[o] = NULL;
		audio_stream_dtor(&vc->sc_rstreams[o]);
	}
	vc->sc_nrfilters = 0;

	if (flags & FREAD)
		sc->sc_recopens--;
	sc->sc_opens--;
	sc->sc_nmixer_states -= 2;
	sc->sc_audiopid[n].pid = -1;
	sc->sc_despid[n].pid = -1;
	mutex_exit(sc->sc_intr_lock);
	mutex_exit(sc->sc_lock);
	audio_free_ring(sc, &vc->sc_mpr);
	audio_free_ring(sc, &vc->sc_mrr);
	mutex_enter(sc->sc_lock);
	kmem_free(sc->sc_vchan[n], sizeof(struct virtual_channel));

	return 0;
}

int
audio_read(struct audio_softc *sc, struct uio *uio, int ioflag)
{
	struct audio_ringbuffer *cb;
	struct virtual_channel *vc;
	const uint8_t *outp;
	uint8_t *inp;
	int error, used, cc, n, m;

	KASSERT(mutex_owned(sc->sc_lock));

	for (m = 1; m < VAUDIOCHANS; m++) {
		if (sc->sc_audiopid[m].pid == curproc->p_pid)
			break;
	}
	if (m == VAUDIOCHANS)
		return EINVAL;

	if (sc->hw_if == NULL)
		return ENXIO;
	vc = sc->sc_vchan[m];

	cb = &vc->sc_mrr;
	if (cb->mmapped)
		return EINVAL;

	DPRINTFN(1,("audio_read: cc=%zu mode=%d\n",
		    uio->uio_resid, vc->sc_mode));

#ifdef AUDIO_PM_IDLE
	if (device_is_active(&sc->dev) || sc->sc_idle)
		device_active(&sc->dev, DVA_SYSTEM);
#endif

	error = 0;
	/*
	 * If hardware is half-duplex and currently playing, return
	 * silence blocks based on the number of blocks we have output.
	 */
	if (!vc->sc_full_duplex && (vc->sc_mode & AUMODE_PLAY)) {
		while (uio->uio_resid > 0 && !error) {
			for(;;) {
				/*
				 * No need to lock, as any wakeup will be
				 * held for us while holding sc_lock.
				 */
				cc = vc->sc_mpr.stamp - vc->sc_wstamp;
				if (cc > 0)
					break;
				DPRINTF(("audio_read: stamp=%lu, wstamp=%lu\n",
					 vc->sc_mpr.stamp, vc->sc_wstamp));
				if (ioflag & IO_NDELAY)
					return EWOULDBLOCK;
				error = audio_waitio(sc, &sc->sc_rchan);
				if (sc->sc_dying)
					error = EIO;
				if (error)
					return error;
			}

			if (uio->uio_resid < cc)
				cc = uio->uio_resid;
			DPRINTFN(1,("audio_read: reading in write mode, "
				    "cc=%d\n", cc));
			error = audio_silence_copyout(sc, cc, uio);
			vc->sc_wstamp += cc;
		}
		return error;
	}

	mutex_enter(sc->sc_intr_lock);
	while (uio->uio_resid > 0 && !error) {
		while ((used = audio_stream_get_used(vc->sc_rustream)) <= 0) {
			if (!vc->sc_rbus && !vc->sc_mrr.pause)
				error = audiostartr(sc, m);
			mutex_exit(sc->sc_intr_lock);
			if (error)
				return error;
			if (ioflag & IO_NDELAY)
				return EWOULDBLOCK;
			DPRINTFN(2, ("audio_read: sleep used=%d\n", used));
			error = audio_waitio(sc, &sc->sc_rchan);
			if (sc->sc_dying)
				error = EIO;
			if (error)
				return error;
			mutex_enter(sc->sc_intr_lock);
		}

		outp = vc->sc_rustream->outp;
		inp = vc->sc_rustream->inp;
		cb->copying = true;

		/*
		 * cc is the amount of data in the sc_rustream excluding
		 * wrapped data.  Note the tricky case of inp == outp, which
		 * must mean the buffer is full, not empty, because used > 0.
		 */
		cc = outp < inp ? inp - outp :vc->sc_rustream->end - outp;
		DPRINTFN(1,("audio_read: outp=%p, cc=%d\n", outp, cc));

		n = uio->uio_resid;
		mutex_exit(sc->sc_intr_lock);
		mutex_exit(sc->sc_lock);
		error = uiomove(__UNCONST(outp), cc, uio);
		mutex_enter(sc->sc_lock);
		mutex_enter(sc->sc_intr_lock);
		n -= uio->uio_resid; /* number of bytes actually moved */

		vc->sc_rustream->outp = audio_stream_add_outp
			(vc->sc_rustream, outp, n);
		cb->copying = false;
	}
	mutex_exit(sc->sc_intr_lock);
	return error;
}

void
audio_clear(struct audio_softc *sc, int n)
{

	struct virtual_channel *vc = sc->sc_vchan[n];
 
	KASSERT(mutex_owned(sc->sc_intr_lock));

	if (vc->sc_rbus) {
		cv_broadcast(&sc->sc_rchan);
		if (sc->sc_recopens == 1) {
			sc->hw_if->halt_input(sc->hw_hdl);
			sc->sc_rec_started = false;
		}
		vc->sc_rbus = false;
		vc->sc_mrr.pause = false;
	}
	if (vc->sc_pbus) {
		cv_broadcast(&sc->sc_wchan);
		vc->sc_pbus = false;
		vc->sc_mpr.pause = false;
	}
}

void
audio_clear_intr_unlocked(struct audio_softc *sc, int n)
{

	mutex_enter(sc->sc_intr_lock);
	audio_clear(sc, n);
	mutex_exit(sc->sc_intr_lock);
}

void
audio_calc_blksize(struct audio_softc *sc, int mode, int n)
{
	const audio_params_t *parm;
	struct audio_ringbuffer *rb;

	if (sc->sc_vchan[n]->sc_blkset)
		return;

	if (mode == AUMODE_PLAY) {
		rb = &sc->sc_vchan[n]->sc_mpr;
		parm = &rb->s.param;
	} else {
		rb = &sc->sc_vchan[n]->sc_mrr;
		parm = &rb->s.param;
	}

	rb->blksize = parm->sample_rate * audio_blk_ms / 1000 *
	     parm->channels * parm->precision / NBBY;

	DPRINTF(("audio_calc_blksize: %s blksize=%d\n",
		 mode == AUMODE_PLAY ? "play" : "record", rb->blksize));
}

void
audio_fill_silence(struct audio_params *params, uint8_t *p, int n)
{
	uint8_t auzero0, auzero1;
	int nfill;

	auzero1 = 0;		/* initialize to please gcc */
	nfill = 1;
	switch (params->encoding) {
	case AUDIO_ENCODING_ULAW:
		auzero0 = 0x7f;
		break;
	case AUDIO_ENCODING_ALAW:
		auzero0 = 0x55;
		break;
	case AUDIO_ENCODING_MPEG_L1_STREAM:
	case AUDIO_ENCODING_MPEG_L1_PACKETS:
	case AUDIO_ENCODING_MPEG_L1_SYSTEM:
	case AUDIO_ENCODING_MPEG_L2_STREAM:
	case AUDIO_ENCODING_MPEG_L2_PACKETS:
	case AUDIO_ENCODING_MPEG_L2_SYSTEM:
	case AUDIO_ENCODING_AC3:
	case AUDIO_ENCODING_ADPCM: /* is this right XXX */
	case AUDIO_ENCODING_SLINEAR_LE:
	case AUDIO_ENCODING_SLINEAR_BE:
		auzero0 = 0;/* fortunately this works for any number of bits */
		break;
	case AUDIO_ENCODING_ULINEAR_LE:
	case AUDIO_ENCODING_ULINEAR_BE:
		if (params->precision > 8) {
			nfill = (params->precision + NBBY - 1)/ NBBY;
			auzero0 = 0x80;
			auzero1 = 0;
		} else
			auzero0 = 0x80;
		break;
	default:
		DPRINTF(("audio: bad encoding %d\n", params->encoding));
		auzero0 = 0;
		break;
	}
	if (nfill == 1) {
		while (--n >= 0)
			*p++ = auzero0; /* XXX memset */
	} else /* nfill must no longer be 2 */ {
		if (params->encoding == AUDIO_ENCODING_ULINEAR_LE) {
			int k = nfill;
			while (--k > 0)
				*p++ = auzero1;
			n -= nfill - 1;
		}
		while (n >= nfill) {
			int k = nfill;
			*p++ = auzero0;
			while (--k > 0)
				*p++ = auzero1;

			n -= nfill;
		}
		if (n-- > 0)	/* XXX must be 1 - DIAGNOSTIC check? */
			*p++ = auzero0;
	}
}

int
audio_silence_copyout(struct audio_softc *sc, int n, struct uio *uio)
{
	struct virtual_channel *vc;
	uint8_t zerobuf[128];
	int error;
	int k;

	vc = sc->sc_vchan[0];
	audio_fill_silence(&vc->sc_rparams, zerobuf, sizeof zerobuf);

	error = 0;
	while (n > 0 && uio->uio_resid > 0 && !error) {
		k = min(n, min(uio->uio_resid, sizeof zerobuf));
		mutex_exit(sc->sc_lock);
		error = uiomove(zerobuf, k, uio);
		mutex_enter(sc->sc_lock);
		n -= k;
	}

	return error;
}

static int
uio_fetcher_fetch_to(struct audio_softc *sc, stream_fetcher_t *self,
    audio_stream_t *p, int max_used)
{
	uio_fetcher_t *this;
	int size;
	int stream_space;
	int error;

	KASSERT(mutex_owned(sc->sc_lock));
	KASSERT(!cpu_intr_p());
	KASSERT(!cpu_softintr_p());

	this = (uio_fetcher_t *)self;
	this->last_used = audio_stream_get_used(p);
	if (this->last_used >= this->usedhigh)
		return 0;
	/*
	 * uio_fetcher ignores max_used and move the data as
	 * much as possible in order to return the correct value
	 * for audio_prinfo::seek and kfilters.
	 */
	stream_space = audio_stream_get_space(p);
	size = min(this->uio->uio_resid, stream_space);

	/* the first fragment of the space */
	stream_space = p->end - p->inp;
	if (stream_space >= size) {
		mutex_exit(sc->sc_lock);
		error = uiomove(p->inp, size, this->uio);
		mutex_enter(sc->sc_lock);
		if (error)
			return error;
		p->inp = audio_stream_add_inp(p, p->inp, size);
	} else {
		mutex_exit(sc->sc_lock);
		error = uiomove(p->inp, stream_space, this->uio);
		mutex_enter(sc->sc_lock);
		if (error)
			return error;
		p->inp = audio_stream_add_inp(p, p->inp, stream_space);
		mutex_exit(sc->sc_lock);
		error = uiomove(p->start, size - stream_space, this->uio);
		mutex_enter(sc->sc_lock);
		if (error)
			return error;
		p->inp = audio_stream_add_inp(p, p->inp, size - stream_space);
	}
	this->last_used = audio_stream_get_used(p);
	return 0;
}

static int
null_fetcher_fetch_to(struct audio_softc *sc, stream_fetcher_t *self,
    audio_stream_t *p, int max_used)
{

	return 0;
}

static void
uio_fetcher_ctor(uio_fetcher_t *this, struct uio *u, int h)
{

	this->base.fetch_to = uio_fetcher_fetch_to;
	this->uio = u;
	this->usedhigh = h;
}

int
audio_write(struct audio_softc *sc, struct uio *uio, int ioflag)
{
	uio_fetcher_t ufetcher;
	audio_stream_t stream;
	struct audio_ringbuffer *cb;
	struct virtual_channel *vc;
	stream_fetcher_t *fetcher;
	stream_filter_t *filter;
	uint8_t *inp, *einp;
	int saveerror, error, n, m, cc, used;

	KASSERT(mutex_owned(sc->sc_lock));

	for (n = 1; n < VAUDIOCHANS; n++) {
		if (sc->sc_audiopid[n].pid == curproc->p_pid)
			break;
	}
	if (n == VAUDIOCHANS)
		return EINVAL;

	if (sc->hw_if == NULL)
		return ENXIO;

	vc = sc->sc_vchan[n];
	cb = &vc->sc_mpr;

	DPRINTFN(2,("audio_write: sc=%p count=%zu used=%d(hi=%d)\n",
		    sc, uio->uio_resid, audio_stream_get_used(vc->sc_pustream),
		    vc->sc_mpr.usedhigh));
	if (vc->sc_mpr.mmapped)
		return EINVAL;

	if (uio->uio_resid == 0) {
		sc->sc_eof++;
		return 0;
	}

#ifdef AUDIO_PM_IDLE
	if (device_is_active(&sc->dev) || sc->sc_idle)
		device_active(&sc->dev, DVA_SYSTEM);
#endif

	/*
	 * If half-duplex and currently recording, throw away data.
	 */
	if (!vc->sc_full_duplex &&
	    (vc->sc_mode & AUMODE_RECORD)) {
		uio->uio_offset += uio->uio_resid;
		uio->uio_resid = 0;
		DPRINTF(("audio_write: half-dpx read busy\n"));
		return 0;
	}

	if (!(vc->sc_mode & AUMODE_PLAY_ALL) && vc->sc_playdrop > 0) {
		m = min(vc->sc_playdrop, uio->uio_resid);
		DPRINTF(("audio_write: playdrop %d\n", m));
		uio->uio_offset += m;
		uio->uio_resid -= m;
		vc->sc_playdrop -= m;
		if (uio->uio_resid == 0)
			return 0;
	}

	/**
	 * setup filter pipeline
	 */
	uio_fetcher_ctor(&ufetcher, uio, vc->sc_mpr.usedhigh);
	if (vc->sc_npfilters > 0) {
		fetcher = &vc->sc_pfilters[vc->sc_npfilters - 1]->base;
	} else {
		fetcher = &ufetcher.base;
	}

	error = 0;
	mutex_enter(sc->sc_intr_lock);
	while (uio->uio_resid > 0 && !error) {
		/* wait if the first buffer is occupied */
		while ((used = audio_stream_get_used(vc->sc_pustream)) >=
							 cb->usedhigh) {
			DPRINTFN(2, ("audio_write: sleep used=%d lowat=%d "
				     "hiwat=%d\n", used,
				     cb->usedlow, cb->usedhigh));
			mutex_exit(sc->sc_intr_lock);
			if (ioflag & IO_NDELAY)
				return EWOULDBLOCK;
			error = audio_waitio(sc, &sc->sc_wchan);
			if (sc->sc_dying)
				error = EIO;
			if (error)
				return error;
			mutex_enter(sc->sc_intr_lock);
		}
		inp = cb->s.inp;
		cb->copying = true;
		stream = cb->s;
		used = stream.used;

		/* Write to the sc_pustream as much as possible. */
		mutex_exit(sc->sc_intr_lock);
		if (vc->sc_npfilters > 0) {
			filter = vc->sc_pfilters[0];
			filter->set_fetcher(filter, &ufetcher.base);
			fetcher = &vc->sc_pfilters[vc->sc_npfilters - 1]->base;
			cc = cb->blksize * 2;
			error = fetcher->fetch_to(sc, fetcher, &stream, cc);
			if (error != 0) {
				fetcher = &ufetcher.base;
				cc = vc->sc_pustream->end -
				    vc->sc_pustream->start;
				error = fetcher->fetch_to(sc, fetcher,
				    vc->sc_pustream, cc);
			}
		} else {
			fetcher = &ufetcher.base;
			cc = stream.end - stream.start;
			error = fetcher->fetch_to(sc, fetcher, &stream, cc);
		}
		mutex_enter(sc->sc_intr_lock);
		if (vc->sc_npfilters > 0) {
			cb->fstamp += ufetcher.last_used
			    - audio_stream_get_used(vc->sc_pustream);
		}
		cb->s.used += stream.used - used;
		cb->s.inp = stream.inp;
		einp = cb->s.inp;

		/*
		 * This is a very suboptimal way of keeping track of
		 * silence in the buffer, but it is simple.
		 */
		vc->sc_sil_count = 0;

		/*
		 * If the interrupt routine wants the last block filled AND
		 * the copy did not fill the last block completely it needs to
		 * be padded.
		 */
		if (cb->needfill && inp < einp &&
		    (inp  - cb->s.start) / cb->blksize ==
		    (einp - cb->s.start) / cb->blksize) {
			/* Figure out how many bytes to a block boundary. */
			cc = cb->blksize - (einp - cb->s.start) % cb->blksize;
			DPRINTF(("audio_write: partial fill %d\n", cc));
		} else
			cc = 0;
		cb->needfill = false;
		cb->copying = false;
		if (!vc->sc_pbus && !cb->pause) {
			saveerror = error;
			error = audiostartp(sc, n);
			if (saveerror != 0) {
				/* Report the first error that occurred. */
				error = saveerror;
			}
		}
		if (cc != 0) {
			DPRINTFN(1, ("audio_write: fill %d\n", cc));
			audio_fill_silence(&cb->s.param, einp, cc);
		}
	}
	mutex_exit(sc->sc_intr_lock);

	return error;
}

int
audio_ioctl(dev_t dev, struct audio_softc *sc, u_long cmd, void *addr, int flag,
	    struct lwp *l)
{
	const struct audio_hw_if *hw;
	struct virtual_channel *vc;
	struct audio_offset *ao;
	u_long stamp;
	int error, offs, fd, m, n;
	bool rbus, pbus;

	KASSERT(mutex_owned(sc->sc_lock));
	for (n = 1; n < VAUDIOCHANS; n++) {
		if (sc->sc_audiopid[n].pid == curproc->p_pid)
			break;
	}
	m = n;
	for (n = 1; n < VAUDIOCHANS; n++) {
		if (sc->sc_despid[m].pid >= 0 && sc->sc_audiopid[n].pid ==
		    sc->sc_despid[m].pid)
				break;
	}

	if (m >= VAUDIOCHANS || n >= VAUDIOCHANS)
		return ENXIO;

	vc = sc->sc_vchan[n];

	DPRINTF(("audio_ioctl(%lu,'%c',%lu)\n",
		 IOCPARM_LEN(cmd), (char)IOCGROUP(cmd), cmd&0xff));
	hw = sc->hw_if;
	if (hw == NULL)
		return ENXIO;
	error = 0;
	switch (cmd) {
	case AUDIO_SETPROC:
		if ((struct audio_pid *)addr != NULL)
			sc->sc_despid[m] = *(struct audio_pid *)addr;
		break;
	case FIONBIO:
		/* All handled in the upper FS layer. */
		break;

	case FIONREAD:
		*(int *)addr = audio_stream_get_used(vc->sc_rustream);
		break;

	case FIOASYNC:
		if (*(int *)addr) {
			if (sc->sc_async_audio != 0)
				error = EBUSY;
			else
				sc->sc_async_audio = sc->sc_audiopid[n].pid;
			DPRINTF(("audio_ioctl: FIOASYNC pid %d\n",
			    sc->sc_audiopid[n].pid));
		} else
			sc->sc_async_audio = 0;
		break;

	case AUDIO_FLUSH:
		DPRINTF(("AUDIO_FLUSH\n"));
		rbus = vc->sc_rbus;
		pbus = vc->sc_pbus;
		mutex_enter(sc->sc_intr_lock);
		audio_clear(sc, n);
		error = audio_initbufs(sc, n);
		if (error) {
			mutex_exit(sc->sc_intr_lock);
			return error;
		}
		if ((vc->sc_mode & AUMODE_PLAY) && !vc->sc_pbus && pbus)
			error = audiostartp(sc, n);
		if (!error &&
		    (vc->sc_mode & AUMODE_RECORD) && !vc->sc_rbus && rbus)
			error = audiostartr(sc, n);
		mutex_exit(sc->sc_intr_lock);
		break;

	/*
	 * Number of read (write) samples dropped.  We don't know where or
	 * when they were dropped.
	 */
	case AUDIO_RERROR:
		*(int *)addr = vc->sc_mrr.drops;
		break;

	case AUDIO_PERROR:
		*(int *)addr = vc->sc_mpr.drops;
		break;

	/*
	 * Offsets into buffer.
	 */
	case AUDIO_GETIOFFS:
		ao = (struct audio_offset *)addr;
		mutex_enter(sc->sc_intr_lock);
		/* figure out where next DMA will start */
		stamp = vc->sc_rustream == &vc->sc_mrr.s
			? vc->sc_mrr.stamp : vc->sc_mrr.fstamp;
		offs = vc->sc_rustream->inp - vc->sc_rustream->start;
		mutex_exit(sc->sc_intr_lock);
		ao->samples = stamp;
		ao->deltablks =
		  (stamp / vc->sc_mrr.blksize) -
		  (vc->sc_mrr.stamp_last / vc->sc_mrr.blksize);
		vc->sc_mrr.stamp_last = stamp;
		ao->offset = offs;
		break;

	case AUDIO_GETOOFFS:
		ao = (struct audio_offset *)addr;
		mutex_enter(sc->sc_intr_lock);
		/* figure out where next DMA will start */
		stamp = vc->sc_pustream == &vc->sc_mpr.s
			? vc->sc_mpr.stamp : vc->sc_mpr.fstamp;
		offs = vc->sc_pustream->outp - vc->sc_pustream->start
			+ vc->sc_mpr.blksize;
		mutex_exit(sc->sc_intr_lock);
		ao->samples = stamp;
		ao->deltablks =
		  (stamp / vc->sc_mpr.blksize) -
		  (vc->sc_mpr.stamp_last / vc->sc_mpr.blksize);
		vc->sc_mpr.stamp_last = stamp;
		if (vc->sc_pustream->start + offs >= vc->sc_pustream->end)
			offs = 0;
		ao->offset = offs;
		break;

	/*
	 * How many bytes will elapse until mike hears the first
	 * sample of what we write next?
	 */
	case AUDIO_WSEEK:
		*(u_long *)addr = audio_stream_get_used(vc->sc_pustream);
		break;

	case AUDIO_SETINFO:
		DPRINTF(("AUDIO_SETINFO mode=0x%x\n", vc->sc_mode));
		error = audiosetinfo(sc, (struct audio_info *)addr, false, n);
		if (!error && ISDEVSOUND(dev)) {
			error = audiogetinfo(sc, &sc->sc_ai, 0, n);
			sc->sc_aivalid = true;
		}
		break;

	case AUDIO_GETINFO:
		DPRINTF(("AUDIO_GETINFO\n"));
		error = audiogetinfo(sc, (struct audio_info *)addr, 0, n);
		break;

	case AUDIO_GETBUFINFO:
		DPRINTF(("AUDIO_GETBUFINFO\n"));
		error = audiogetinfo(sc, (struct audio_info *)addr, 1, n);
		break;

	case AUDIO_DRAIN:
		DPRINTF(("AUDIO_DRAIN\n"));
		mutex_enter(sc->sc_intr_lock);
		error = audio_drain(sc, n);
		if (!error && sc->sc_opens == 1 && hw->drain)
		    error = hw->drain(sc->hw_hdl);
		mutex_exit(sc->sc_intr_lock);
		break;

	case AUDIO_GETDEV:
		DPRINTF(("AUDIO_GETDEV\n"));
		error = hw->getdev(sc->hw_hdl, (audio_device_t *)addr);
		break;

	case AUDIO_GETENC:
		DPRINTF(("AUDIO_GETENC\n"));
		error = audio_query_encoding(sc,
		    (struct audio_encoding *)addr);
		break;

	case AUDIO_GETFD:
		DPRINTF(("AUDIO_GETFD\n"));
		*(int *)addr = vc->sc_full_duplex;
		break;

	case AUDIO_SETFD:
		DPRINTF(("AUDIO_SETFD\n"));
		fd = *(int *)addr;
		if (audio_get_props(sc) & AUDIO_PROP_FULLDUPLEX) {
			if (hw->setfd)
				error = hw->setfd(sc->hw_hdl, fd);
			else
				error = 0;
			if (!error)
				vc->sc_full_duplex = fd;
		} else {
			if (fd)
				error = ENOTTY;
			else
				error = 0;
		}
		break;

	case AUDIO_GETPROPS:
		DPRINTF(("AUDIO_GETPROPS\n"));
		*(int *)addr = audio_get_props(sc);
		break;

	default:
		if (hw->dev_ioctl) {
			error = hw->dev_ioctl(sc->hw_hdl, cmd, addr, flag, l);
		} else {
			DPRINTF(("audio_ioctl: unknown ioctl\n"));
			error = EINVAL;
		}
		break;
	}
	DPRINTF(("audio_ioctl(%lu,'%c',%lu) result %d\n",
		 IOCPARM_LEN(cmd), (char)IOCGROUP(cmd), cmd&0xff, error));
	return error;
}

int
audio_poll(struct audio_softc *sc, int events, struct lwp *l)
{
	struct virtual_channel *vc;
	int revents;
	int used, n;

	KASSERT(mutex_owned(sc->sc_lock));
	for (n = 1; n < VAUDIOCHANS; n++) {
		if (sc->sc_audiopid[n].pid == curproc->p_pid)
			break;
	}
	if (n == VAUDIOCHANS)
		return ENXIO;
	vc = sc->sc_vchan[n];

	DPRINTF(("audio_poll: events=0x%x mode=%d\n", events, vc->sc_mode));

	revents = 0;
	mutex_enter(sc->sc_intr_lock);
	if (events & (POLLIN | POLLRDNORM)) {
		used = audio_stream_get_used(vc->sc_rustream);
		/*
		 * If half duplex and playing, audio_read() will generate
		 * silence at the play rate; poll for silence being
		 * available.  Otherwise, poll for recorded sound.
		 */
		if ((!vc->sc_full_duplex && (vc->sc_mode & AUMODE_PLAY))
		     ? vc->sc_mpr.stamp > vc->sc_wstamp :
		    used > vc->sc_mrr.usedlow)
			revents |= events & (POLLIN | POLLRDNORM);
	}

	if (events & (POLLOUT | POLLWRNORM)) {
		used = audio_stream_get_used(vc->sc_pustream);
		/*
		 * If half duplex and recording, audio_write() will throw
		 * away play data, which means we are always ready to write.
		 * Otherwise, poll for play buffer being below its low water
		 * mark.
		 */
		if ((!vc->sc_full_duplex && (vc->sc_mode & AUMODE_RECORD)) ||
		    (!(vc->sc_mode & AUMODE_PLAY_ALL) && vc->sc_playdrop > 0) ||
		    (used <= vc->sc_mpr.usedlow))
			revents |= events & (POLLOUT | POLLWRNORM);
	}
	mutex_exit(sc->sc_intr_lock);

	if (revents == 0) {
		if (events & (POLLIN | POLLRDNORM))
			selrecord(l, &sc->sc_rsel);

		if (events & (POLLOUT | POLLWRNORM))
			selrecord(l, &sc->sc_wsel);
	}

	return revents;
}

static void
filt_audiordetach(struct knote *kn)
{
	struct audio_softc *sc;

	sc = kn->kn_hook;
	mutex_enter(sc->sc_intr_lock);
	SLIST_REMOVE(&sc->sc_rsel.sel_klist, kn, knote, kn_selnext);
	mutex_exit(sc->sc_intr_lock);
}

static int
filt_audioread(struct knote *kn, long hint)
{
	struct audio_softc *sc;
	struct virtual_channel *vc;
	int n;

	sc = kn->kn_hook;
	for (n = 1; n < VAUDIOCHANS; n++) {
		if (sc->sc_audiopid[n].pid == curproc->p_pid)
			break;
	}
	if (n == VAUDIOCHANS)
		return ENXIO;

	vc = sc->sc_vchan[n];
	mutex_enter(sc->sc_intr_lock);
	if (!vc->sc_full_duplex && (vc->sc_mode & AUMODE_PLAY))
		kn->kn_data = vc->sc_mpr.stamp - vc->sc_wstamp;
	else
		kn->kn_data = audio_stream_get_used(vc->sc_rustream)
			- vc->sc_mrr.usedlow;
	mutex_exit(sc->sc_intr_lock);

	return kn->kn_data > 0;
}

static const struct filterops audioread_filtops =
	{ 1, NULL, filt_audiordetach, filt_audioread };

static void
filt_audiowdetach(struct knote *kn)
{
	struct audio_softc *sc;

	sc = kn->kn_hook;
	mutex_enter(sc->sc_intr_lock);
	SLIST_REMOVE(&sc->sc_wsel.sel_klist, kn, knote, kn_selnext);
	mutex_exit(sc->sc_intr_lock);
}

static int
filt_audiowrite(struct knote *kn, long hint)
{
	struct audio_softc *sc;
	audio_stream_t *stream;
	int n;

	sc = kn->kn_hook;
	mutex_enter(sc->sc_intr_lock);
	for (n = 1; n < VAUDIOCHANS; n++) {
		if (sc->sc_audiopid[n].pid == curproc->p_pid)
			break;
	}
	if (n == VAUDIOCHANS)
		return ENXIO;

	stream = sc->sc_vchan[n]->sc_pustream;
	kn->kn_data = (stream->end - stream->start)
		- audio_stream_get_used(stream);
	mutex_exit(sc->sc_intr_lock);

	return kn->kn_data > 0;
}

static const struct filterops audiowrite_filtops =
	{ 1, NULL, filt_audiowdetach, filt_audiowrite };

int
audio_kqfilter(struct audio_softc *sc, struct knote *kn)
{
	struct klist *klist;

	switch (kn->kn_filter) {
	case EVFILT_READ:
		klist = &sc->sc_rsel.sel_klist;
		kn->kn_fop = &audioread_filtops;
		break;

	case EVFILT_WRITE:
		klist = &sc->sc_wsel.sel_klist;
		kn->kn_fop = &audiowrite_filtops;
		break;

	default:
		return EINVAL;
	}

	kn->kn_hook = sc;

	mutex_enter(sc->sc_intr_lock);
	SLIST_INSERT_HEAD(klist, kn, kn_selnext);
	mutex_exit(sc->sc_intr_lock);

	return 0;
}

paddr_t
audio_mmap(struct audio_softc *sc, off_t off, int prot)
{
	struct audio_ringbuffer *cb;
	struct virtual_channel *vc;
	paddr_t rv;
	int n;

	KASSERT(mutex_owned(sc->sc_lock));

	for (n = 1; n < VAUDIOCHANS; n++) {
		if (sc->sc_audiopid[n].pid == curproc->p_pid)
			break;
	}
	if (n == VAUDIOCHANS)
		return -1;

	if (sc->hw_if == NULL)
		return ENXIO;

	DPRINTF(("audio_mmap: off=%lld, prot=%d\n", (long long)off, prot));
	vc = sc->sc_vchan[n];
	if (!(audio_get_props(sc) & AUDIO_PROP_MMAP))
		return -1;
#if 0
/* XXX
 * The idea here was to use the protection to determine if
 * we are mapping the read or write buffer, but it fails.
 * The VM system is broken in (at least) two ways.
 * 1) If you map memory VM_PROT_WRITE you SIGSEGV
 *    when writing to it, so VM_PROT_READ|VM_PROT_WRITE
 *    has to be used for mmapping the play buffer.
 * 2) Even if calling mmap() with VM_PROT_READ|VM_PROT_WRITE
 *    audio_mmap will get called at some point with VM_PROT_READ
 *    only.
 * So, alas, we always map the play buffer for now.
 */
	if (prot == (VM_PROT_READ|VM_PROT_WRITE) ||
	    prot == VM_PROT_WRITE)
		cb = &sc->sc_vchan[n]->sc_mpr;
	else if (prot == VM_PROT_READ)
		cb = &sc->sc_vchan[n]->sc_mrr;
	else
		return -1;
#else
	cb = &sc->sc_vchan[n]->sc_mpr;
#endif

	if ((u_int)off >= cb->s.bufsize)
		return -1;
	if (!cb->mmapped) {
		cb->mmapped = true;
		if (cb != &sc->sc_rr) {
			audio_fill_silence(&cb->s.param, cb->s.start,
					   cb->s.bufsize);
			mutex_enter(sc->sc_intr_lock);
			vc->sc_pustream = &cb->s;
			if (!vc->sc_pbus && !vc->sc_mpr.pause)
				(void)audiostartp(sc, n);
			mutex_exit(sc->sc_intr_lock);
		} else {
			mutex_enter(sc->sc_intr_lock);
			vc->sc_rustream = &cb->s;
			if (!vc->sc_rbus && !sc->sc_rr.pause)
				(void)audiostartr(sc, n);
			mutex_exit(sc->sc_intr_lock);
		}
	}

	rv = (paddr_t)(uintptr_t)(cb->s.start + off);

	return rv;
}

int
audiostartr(struct audio_softc *sc, int n)
{
	struct virtual_channel *vc;

	KASSERT(mutex_owned(sc->sc_lock));
	KASSERT(mutex_owned(sc->sc_intr_lock));

	vc = sc->sc_vchan[n];
	DPRINTF(("audiostartr: start=%p used=%d(hi=%d) mmapped=%d\n",
		 vc->sc_mrr.s.start, audio_stream_get_used(&vc->sc_mrr.s),
		 vc->sc_mrr.usedhigh, vc->sc_mrr.mmapped));

	if (!audio_can_capture(sc))
		return EINVAL;

	if (sc->sc_rec_started == false) {
		mix_read(sc);
		cv_broadcast(&sc->sc_rcondvar);
	}
	vc->sc_rbus = true;

	return 0;
}

int
audiostartp(struct audio_softc *sc, int n)
{
	struct virtual_channel *vc = sc->sc_vchan[n];
	int error, used;

	KASSERT(mutex_owned(sc->sc_lock));
	KASSERT(mutex_owned(sc->sc_intr_lock));

	error = 0;
	used = audio_stream_get_used(&vc->sc_mpr.s);
	DPRINTF(("audiostartp: start=%p used=%d(hi=%d blk=%d) mmapped=%d\n",
		 vc->sc_mpr.s.start, used, vc->sc_mpr.usedhigh,
		 vc->sc_mpr.blksize, vc->sc_mpr.mmapped));

	if (!audio_can_playback(sc))
		return EINVAL;

	if (!vc->sc_mpr.mmapped && used < vc->sc_mpr.blksize) {
		cv_broadcast(&sc->sc_wchan);
		DPRINTF(("%s: wakeup and return\n", __func__));
		return 0;
	}
	
	vc->sc_pbus = true;
	if (sc->sc_trigger_started == false) {
		audio_mix(sc);
		mix_write(sc);

		vc = sc->sc_vchan[0];
		vc->sc_mpr.s.outp =
		    audio_stream_add_outp(&vc->sc_mpr.s,
		      vc->sc_mpr.s.outp, vc->sc_mpr.blksize);
		audio_mix(sc);
		mix_write(sc);
		cv_broadcast(&sc->sc_condvar);
	}

	return error;
}

/*
 * When the play interrupt routine finds that the write isn't keeping
 * the buffer filled it will insert silence in the buffer to make up
 * for this.  The part of the buffer that is filled with silence
 * is kept track of in a very approximate way: it starts at sc_sil_start
 * and extends sc_sil_count bytes.  If there is already silence in
 * the requested area nothing is done; so when the whole buffer is
 * silent nothing happens.  When the writer starts again sc_sil_count
 * is set to 0.
 *
 * XXX
 * Putting silence into the output buffer should not really be done
 * from the device interrupt handler.  Consider deferring to the soft
 * interrupt.
 */
static inline void
audio_pint_silence(struct audio_softc *sc, struct audio_ringbuffer *cb,
		   uint8_t *inp, int cc, int n)
{
	struct virtual_channel *vc = sc->sc_vchan[n];
	uint8_t *s, *e, *p, *q;

	KASSERT(mutex_owned(sc->sc_lock));

	if (vc->sc_sil_count > 0) {
		s = vc->sc_sil_start; /* start of silence */
		e = s + vc->sc_sil_count; /* end of sil., may be beyond end */
		p = inp;	/* adjusted pointer to area to fill */
		if (p < s)
			p += cb->s.end - cb->s.start;
		q = p + cc;
		/* Check if there is already silence. */
		if (!(s <= p && p <  e &&
		      s <= q && q <= e)) {
			if (s <= p)
				vc->sc_sil_count = max(vc->sc_sil_count, q-s);
			DPRINTFN(5,("audio_pint_silence: fill cc=%d inp=%p, "
				    "count=%d size=%d\n",
				    cc, inp, vc->sc_sil_count,
				    (int)(cb->s.end - cb->s.start)));
			audio_fill_silence(&cb->s.param, inp, cc);
		} else {
			DPRINTFN(5,("audio_pint_silence: already silent "
				    "cc=%d inp=%p\n", cc, inp));

		}
	} else {
		vc->sc_sil_start = inp;
		vc->sc_sil_count = cc;
		DPRINTFN(5, ("audio_pint_silence: start fill %p %d\n",
			     inp, cc));
		audio_fill_silence(&cb->s.param, inp, cc);
	}
}

static void
audio_softintr_rd(void *cookie)
{
	struct audio_softc *sc = cookie;
	proc_t *p;
	pid_t pid;

	mutex_enter(sc->sc_lock);
	cv_broadcast(&sc->sc_rchan);
	selnotify(&sc->sc_rsel, 0, NOTE_SUBMIT);
	if ((pid = sc->sc_async_audio) != 0) {
		DPRINTFN(3, ("audio_softintr_rd: sending SIGIO %d\n", pid));
		mutex_enter(proc_lock);
		if ((p = proc_find(pid)) != NULL)
			psignal(p, SIGIO);
		mutex_exit(proc_lock);
	}
	mutex_exit(sc->sc_lock);
}

static void
audio_softintr_wr(void *cookie)
{
	struct audio_softc *sc = cookie;
	proc_t *p;
	pid_t pid;

	mutex_enter(sc->sc_lock);
	cv_broadcast(&sc->sc_wchan);
	selnotify(&sc->sc_wsel, 0, NOTE_SUBMIT);
	if ((pid = sc->sc_async_audio) != 0) {
		DPRINTFN(3, ("audio_softintr_wr: sending SIGIO %d\n", pid));
		mutex_enter(proc_lock);
		if ((p = proc_find(pid)) != NULL)
			psignal(p, SIGIO);
		mutex_exit(proc_lock);
	}
	mutex_exit(sc->sc_lock);
}

/*
 * Called from HW driver module on completion of DMA output.
 * Start output of new block, wrap in ring buffer if needed.
 * If no more buffers to play, output zero instead.
 * Do a wakeup if necessary.
 */
void
audio_pint(void *v)
{
	struct audio_softc *sc;
	struct virtual_channel *vc;

	sc = v;
	vc = sc->sc_vchan[0];

	if (sc->sc_dying == true)
		return;

	if (audio_stream_get_used(&sc->sc_pr.s) < vc->sc_mpr.blksize)
		goto wake_mix;

	vc->sc_mpr.s.outp = audio_stream_add_outp(&vc->sc_mpr.s,
	    vc->sc_mpr.s.outp, vc->sc_mpr.blksize);
	mix_write(sc);

wake_mix:
	cv_broadcast(&sc->sc_condvar);
}

void
audio_mix(void *v)
{
	stream_fetcher_t null_fetcher;
	struct audio_softc *sc;
	struct virtual_channel *vc;
	struct audio_ringbuffer *cb;
	stream_fetcher_t *fetcher;
	uint8_t *inp;
	int cc, used;
	int blksize;
	int n, i;
	
	sc = v;

	DPRINTF(("PINT MIX\n"));
	sc->schedule_rih = false;
	sc->schedule_wih = false;
	sc->sc_writeme = false;

	if (sc->sc_dying == true)
		return;

	i = sc->sc_opens;
	blksize = sc->sc_vchan[0]->sc_mpr.blksize;
	for (n = 1; n < VAUDIOCHANS; n++) {
		if (!sc->sc_opens || i <= 0)
			break;		/* ignore interrupt if not open */

		if (sc->sc_audiopid[n].pid == -1)
			continue;
		i--;
		vc = sc->sc_vchan[n];
		if (!vc->sc_open)
			continue;
		if (!vc->sc_pbus)
			continue;

		cb = &vc->sc_mpr;

		sc->sc_writeme = true;

		inp = cb->s.inp;
		cb->stamp += blksize;
		if (cb->mmapped) {
			DPRINTF(("audio_pint: mmapped outp=%p cc=%d inp=%p\n",
				     cb->s.outp, blksize, cb->s.inp));
			mix_func(sc, cb, n);
			continue;
		}

#ifdef AUDIO_INTR_TIME
		{
			struct timeval tv;
			u_long t;
			microtime(&tv);
			t = tv.tv_usec + 1000000 * tv.tv_sec;
			if (sc->sc_pnintr) {
				long lastdelta, totdelta;
				lastdelta = t - sc->sc_plastintr -
				    sc->sc_pblktime;
				if (lastdelta > sc->sc_pblktime / 3) {
					printf("audio: play interrupt(%d) off "
				       "relative by %ld us (%lu)\n",
					       sc->sc_pnintr, lastdelta,
					       sc->sc_pblktime);
				}
				totdelta = t - sc->sc_pfirstintr -
					sc->sc_pblktime * sc->sc_pnintr;
				if (totdelta > sc->sc_pblktime) {
					printf("audio: play interrupt(%d) "
					       "off absolute by %ld us (%lu) "
					       "(LOST)\n", sc->sc_pnintr,
					       totdelta, sc->sc_pblktime);
					sc->sc_pnintr++;
					/* avoid repeated messages */
				}
			} else
				sc->sc_pfirstintr = t;
			sc->sc_plastintr = t;
			sc->sc_pnintr++;
		}
#endif

		used = audio_stream_get_used(&cb->s);
		/*
		 * "used <= cb->usedlow" should be "used < blksize" ideally.
		 * Some HW drivers such as uaudio(4) does not call audio_pint()
		 * at accurate timing.  If used < blksize, uaudio(4) already
		 * request transfer of garbage data.
		 */
		if (used <= cb->usedlow && !cb->copying &&
		    vc->sc_npfilters > 0) {
			/* we might have data in filter pipeline */
			null_fetcher.fetch_to = null_fetcher_fetch_to;
			fetcher = &vc->sc_pfilters[vc->sc_npfilters - 1]->base;
			vc->sc_pfilters[0]->set_fetcher(vc->sc_pfilters[0],
							&null_fetcher);
			used = audio_stream_get_used(vc->sc_pustream);
			cc = cb->s.end - cb->s.start;
			if (blksize * 2 < cc)
				cc = blksize * 2;
			fetcher->fetch_to(sc, fetcher, &cb->s, cc);
			cb->fstamp += used -
			    audio_stream_get_used(vc->sc_pustream);
			used = audio_stream_get_used(&cb->s);
		}
		if (used < blksize) {
			/* we don't have a full block to use */
			if (cb->copying) {
				/* writer is in progress, don't disturb */
				cb->needfill = true;
				DPRINTFN(1, ("audio_pint: copying in "
					 "progress\n"));
			} else {
				inp = cb->s.inp;
				cc = blksize - (inp - cb->s.start) % blksize;
				if (cb->pause)
					cb->pdrops += cc;
				else {
					cb->drops += cc;
					vc->sc_playdrop += cc;
				}

				audio_pint_silence(sc, cb, inp, cc, n);
				cb->s.inp = audio_stream_add_inp(&cb->s, inp,
				    cc);

				/* Clear next block to keep ahead of the DMA. */
				used = audio_stream_get_used(&cb->s);
				if (used + blksize < cb->s.end - cb->s.start) {
					audio_pint_silence(sc, cb, cb->s.inp,
					    blksize, n);
				}
			}
		}

		DPRINTFN(5, ("audio_pint: outp=%p cc=%d\n", cb->s.outp,
			 blksize));
		mix_func(sc, cb, n);
		cb->s.outp = audio_stream_add_outp(&cb->s, cb->s.outp, blksize);

		DPRINTFN(2, ("audio_pint: mode=%d pause=%d used=%d lowat=%d\n",
			     vc->sc_mode, cb->pause,
			     audio_stream_get_used(vc->sc_pustream),
			     cb->usedlow));

		if ((vc->sc_mode & AUMODE_PLAY) && !cb->pause) {
			if (audio_stream_get_used(&cb->s) <= cb->usedlow)
				sc->schedule_wih = true;
		}
		/* Possible to return one or more "phantom blocks" now. */
		if (!vc->sc_full_duplex && vc->sc_mode & AUMODE_RECORD)
				sc->schedule_rih = true;
	}
	if (sc->sc_saturate == true && sc->sc_opens > 1)
		saturate_func(sc);

	cb = &sc->sc_pr;
	cb->s.inp = audio_stream_add_inp(&cb->s, cb->s.inp, blksize);

	kpreempt_disable();
	if (sc->schedule_wih == true)
		softint_schedule(sc->sc_sih_wr);

	if (sc->schedule_rih == true)
		softint_schedule(sc->sc_sih_rd);
	kpreempt_enable();

	cc = sc->sc_vchan[0]->sc_mpr.blksize;
	if (sc->sc_writeme == false) {
		sc->sc_vchan[0]->sc_mpr.drops += cc;
		cv_broadcast(&sc->sc_wchan);
	}
}

/*
 * Called from HW driver module on completion of DMA input.
 * Mark it as input in the ring buffer (fiddle pointers).
 * Do a wakeup if necessary.
 */
void
audio_rint(void *v)
{
	struct audio_softc *sc;
	int blksize;

	sc = v;

	if (sc->sc_dying == true)
		return;

	blksize = audio_stream_get_used(&sc->sc_rr.s);
	sc->sc_rr.s.outp = audio_stream_add_outp(&sc->sc_rr.s,
	    sc->sc_rr.s.outp, blksize);
	mix_read(sc);

	cv_broadcast(&sc->sc_rcondvar);
}

void
audio_upmix(void *v)
{
	stream_fetcher_t null_fetcher;
	struct audio_softc *sc;
	struct audio_ringbuffer *cb;
	stream_fetcher_t *last_fetcher;
	struct virtual_channel *vc;
	int cc;
	int used;
	int blksize;
	int i;
	int n;
	int cc1;

	sc = v;
	i = sc->sc_opens;
	blksize = sc->sc_vchan[0]->sc_mrr.blksize;

	for (n = 1; n < VAUDIOCHANS; n++) {
		if (!sc->sc_opens || i <= 0)
			break;		/* ignore interrupt if not open */

		if (sc->sc_audiopid[n].pid == -1)
			continue;
		i--;
		vc = sc->sc_vchan[n];
		if (!(vc->sc_open & AUOPEN_READ))
			continue;
		if (!vc->sc_rbus)
			continue;

		cb = &vc->sc_mrr;

		blksize = audio_stream_get_used(&sc->sc_rr.s);
		if (audio_stream_get_space(&cb->s) < blksize) {
			cb->drops += blksize;
			cb->s.outp = audio_stream_add_outp(&cb->s, cb->s.outp,
			    sc->sc_rr.blksize);
			continue;
		}

		cc = blksize;
		if (cb->s.inp + blksize > cb->s.end)
			cc = cb->s.end - cb->s.inp;
		memcpy(cb->s.inp, sc->sc_rr.s.start, cc);
		if (cc < blksize && cc != 0) {
			cc1 = cc;
			cc = blksize - cc;
			memcpy(cb->s.start, sc->sc_rr.s.start + cc1, cc);
		}

		cc = blksize;
		recswvol_func(sc, cb, n, blksize);

		cb->s.inp = audio_stream_add_inp(&cb->s, cb->s.inp, blksize);
		cb->stamp += blksize;
		if (cb->mmapped) {
			DPRINTFN(2, ("audio_rint: mmapped inp=%p cc=%d\n",
			     	cb->s.inp, blksize));
			continue;
		}

#ifdef AUDIO_INTR_TIME
		{
			struct timeval tv;
			u_long t;
			microtime(&tv);
			t = tv.tv_usec + 1000000 * tv.tv_sec;
			if (sc->sc_rnintr) {
				long lastdelta, totdelta;
				lastdelta = t - sc->sc_rlastintr -
				    sc->sc_rblktime;
				if (lastdelta > sc->sc_rblktime / 5) {
					printf("audio: record interrupt(%d) "
					       "off relative by %ld us (%lu)\n",
					       sc->sc_rnintr, lastdelta,
					       sc->sc_rblktime);
				}
				totdelta = t - sc->sc_rfirstintr -
					sc->sc_rblktime * sc->sc_rnintr;
				if (totdelta > sc->sc_rblktime / 2) {
					sc->sc_rnintr++;
					printf("audio: record interrupt(%d) "
					       "off absolute by %ld us (%lu)\n",
					       sc->sc_rnintr, totdelta,
					       sc->sc_rblktime);
					sc->sc_rnintr++;
					/* avoid repeated messages */
				}
			} else
				sc->sc_rfirstintr = t;
			sc->sc_rlastintr = t;
			sc->sc_rnintr++;
		}
#endif

		if (!cb->pause && vc->sc_nrfilters > 0) {
			null_fetcher.fetch_to = null_fetcher_fetch_to;
			last_fetcher =
			    &vc->sc_rfilters[vc->sc_nrfilters - 1]->base;
			vc->sc_rfilters[0]->set_fetcher(vc->sc_rfilters[0],
							&null_fetcher);
			used = audio_stream_get_used(vc->sc_rustream);
			cc = vc->sc_rustream->end - vc->sc_rustream->start;
			last_fetcher->fetch_to
				(sc, last_fetcher, vc->sc_rustream, cc);
			cb->fstamp += audio_stream_get_used(vc->sc_rustream) -
			    used;
			/* XXX what should do for error? */
		}
		used = audio_stream_get_used(&vc->sc_mrr.s);
		if (cb->pause) {
			DPRINTFN(1, ("audio_rint: pdrops %lu\n", cb->pdrops));
			cb->pdrops += blksize;
			cb->s.outp = audio_stream_add_outp(&cb->s, cb->s.outp,
			    blksize);
		} else if (used + blksize > cb->s.end - cb->s.start &&
								!cb->copying) {
			DPRINTFN(1, ("audio_rint: drops %lu\n", cb->drops));
			cb->drops += blksize;
			cb->s.outp = audio_stream_add_outp(&cb->s, cb->s.outp,
			    blksize);
		}
	}
	kpreempt_disable();
	softint_schedule(sc->sc_sih_rd);
	kpreempt_enable();
}

int
audio_check_params(struct audio_params *p)
{

	if (p->encoding == AUDIO_ENCODING_PCM16) {
		if (p->precision == 8)
			p->encoding = AUDIO_ENCODING_ULINEAR;
		else
			p->encoding = AUDIO_ENCODING_SLINEAR;
	} else if (p->encoding == AUDIO_ENCODING_PCM8) {
		if (p->precision == 8)
			p->encoding = AUDIO_ENCODING_ULINEAR;
		else
			return EINVAL;
	}

	if (p->encoding == AUDIO_ENCODING_SLINEAR)
#if BYTE_ORDER == LITTLE_ENDIAN
		p->encoding = AUDIO_ENCODING_SLINEAR_LE;
#else
		p->encoding = AUDIO_ENCODING_SLINEAR_BE;
#endif
	if (p->encoding == AUDIO_ENCODING_ULINEAR)
#if BYTE_ORDER == LITTLE_ENDIAN
		p->encoding = AUDIO_ENCODING_ULINEAR_LE;
#else
		p->encoding = AUDIO_ENCODING_ULINEAR_BE;
#endif

	switch (p->encoding) {
	case AUDIO_ENCODING_ULAW:
	case AUDIO_ENCODING_ALAW:
		if (p->precision != 8)
			return EINVAL;
		break;
	case AUDIO_ENCODING_ADPCM:
		if (p->precision != 4 && p->precision != 8)
			return EINVAL;
		break;
	case AUDIO_ENCODING_SLINEAR_LE:
	case AUDIO_ENCODING_SLINEAR_BE:
	case AUDIO_ENCODING_ULINEAR_LE:
	case AUDIO_ENCODING_ULINEAR_BE:
		/* XXX is: our zero-fill can handle any multiple of 8 */
		if (p->precision !=  8 && p->precision != 16 &&
		    p->precision != 24 && p->precision != 32)
			return EINVAL;
		if (p->precision == 8 && p->encoding ==
		    AUDIO_ENCODING_SLINEAR_BE)
			p->encoding = AUDIO_ENCODING_SLINEAR_LE;
		if (p->precision == 8 && p->encoding ==
		    AUDIO_ENCODING_ULINEAR_BE)
			p->encoding = AUDIO_ENCODING_ULINEAR_LE;
		if (p->validbits > p->precision)
			return EINVAL;
		break;
	case AUDIO_ENCODING_MPEG_L1_STREAM:
	case AUDIO_ENCODING_MPEG_L1_PACKETS:
	case AUDIO_ENCODING_MPEG_L1_SYSTEM:
	case AUDIO_ENCODING_MPEG_L2_STREAM:
	case AUDIO_ENCODING_MPEG_L2_PACKETS:
	case AUDIO_ENCODING_MPEG_L2_SYSTEM:
	case AUDIO_ENCODING_AC3:
		break;
	default:
		return EINVAL;
	}

	/* sanity check # of channels*/
	if (p->channels < 1 || p->channels > AUDIO_MAX_CHANNELS)
		return EINVAL;

	return 0;
}

static int
audio_set_vchan_defaults(struct audio_softc *sc, u_int mode,
     const struct audio_format *format, int n)
{
	struct virtual_channel *vc = sc->sc_vchan[n];
	struct audio_info ai;
	int error;

	KASSERT(mutex_owned(sc->sc_lock));

	sc->sc_vchan_params.sample_rate = sc->sc_iffreq;
#if BYTE_ORDER == LITTLE_ENDIAN
	sc->sc_vchan_params.encoding = AUDIO_ENCODING_SLINEAR_LE;
#else
	sc->sc_vchan_params.encoding = AUDIO_ENCODING_SLINEAR_BE;
#endif
	sc->sc_vchan_params.precision = sc->sc_precision;
	sc->sc_vchan_params.validbits = sc->sc_precision;
	sc->sc_vchan_params.channels = sc->sc_channels;

	/* default parameters */
	vc->sc_rparams = sc->sc_vchan_params;
	vc->sc_pparams = sc->sc_vchan_params;
	vc->sc_blkset = false;

	AUDIO_INITINFO(&ai);
	ai.record.sample_rate = sc->sc_iffreq;
	ai.record.encoding    = format->encoding;
	ai.record.channels    = sc->sc_channels;
	ai.record.precision   = sc->sc_precision;
	ai.record.pause	      = false;
	ai.play.sample_rate   = sc->sc_iffreq;
	ai.play.encoding      = format->encoding;
	ai.play.channels      = sc->sc_channels;
	ai.play.precision     = sc->sc_precision;
	ai.play.pause         = false;
	ai.mode		      = mode;

	sc->sc_format->channels = sc->sc_channels;
	sc->sc_format->precision = sc->sc_precision;
	sc->sc_format->validbits = sc->sc_precision;
	sc->sc_format->frequency[0] = sc->sc_iffreq;

	auconv_delete_encodings(sc->sc_encodings);
	error = auconv_create_encodings(sc->sc_format, VAUDIO_NFORMATS,
	    &sc->sc_encodings);

	if (error == 0)
		error = audiosetinfo(sc, &ai, true, n);

	return error;
}

int
audio_set_defaults(struct audio_softc *sc, u_int mode, int n)
{
	struct virtual_channel *vc = sc->sc_vchan[n];
	struct audio_info ai;

	KASSERT(mutex_owned(sc->sc_lock));

	/* default parameters */
	vc->sc_rparams = audio_default;
	vc->sc_pparams = audio_default;
	vc->sc_blkset = false;

	AUDIO_INITINFO(&ai);
	ai.record.sample_rate = vc->sc_rparams.sample_rate;
	ai.record.encoding    = vc->sc_rparams.encoding;
	ai.record.channels    = vc->sc_rparams.channels;
	ai.record.precision   = vc->sc_rparams.precision;
	ai.record.pause	      = false;
	ai.play.sample_rate   = vc->sc_pparams.sample_rate;
	ai.play.encoding      = vc->sc_pparams.encoding;
	ai.play.channels      = vc->sc_pparams.channels;
	ai.play.precision     = vc->sc_pparams.precision;
	ai.play.pause         = false;
	ai.mode		      = mode;

	return audiosetinfo(sc, &ai, true, n);
}

int
au_set_lr_value(struct	audio_softc *sc, mixer_ctrl_t *ct, int l, int r)
{

	KASSERT(mutex_owned(sc->sc_lock));

	ct->type = AUDIO_MIXER_VALUE;
	ct->un.value.num_channels = 2;
	ct->un.value.level[AUDIO_MIXER_LEVEL_LEFT] = l;
	ct->un.value.level[AUDIO_MIXER_LEVEL_RIGHT] = r;
	if (audio_set_port(sc, ct) == 0)
		return 0;
	ct->un.value.num_channels = 1;
	ct->un.value.level[AUDIO_MIXER_LEVEL_MONO] = (l+r)/2;
	return audio_set_port(sc, ct);
}

int
au_set_gain(struct audio_softc *sc, struct au_mixer_ports *ports,
	    int gain, int balance)
{
	mixer_ctrl_t ct;
	int i, error;
	int l, r;
	u_int mask;
	int nset;

	KASSERT(mutex_owned(sc->sc_lock));

	if (balance == AUDIO_MID_BALANCE) {
		l = r = gain;
	} else if (balance < AUDIO_MID_BALANCE) {
		l = gain;
		r = (balance * gain) / AUDIO_MID_BALANCE;
	} else {
		r = gain;
		l = ((AUDIO_RIGHT_BALANCE - balance) * gain)
		    / AUDIO_MID_BALANCE;
	}
	DPRINTF(("au_set_gain: gain=%d balance=%d, l=%d r=%d\n",
		 gain, balance, l, r));

	if (ports->index == -1) {
	usemaster:
		if (ports->master == -1)
			return 0; /* just ignore it silently */
		ct.dev = ports->master;
		error = au_set_lr_value(sc, &ct, l, r);
	} else {
		ct.dev = ports->index;
		if (ports->isenum) {
			ct.type = AUDIO_MIXER_ENUM;
			error = audio_get_port(sc, &ct);
			if (error)
				return error;
			if (ports->isdual) {
				if (ports->cur_port == -1)
					ct.dev = ports->master;
				else
					ct.dev = ports->miport[ports->cur_port];
				error = au_set_lr_value(sc, &ct, l, r);
			} else {
				for(i = 0; i < ports->nports; i++)
				    if (ports->misel[i] == ct.un.ord) {
					    ct.dev = ports->miport[i];
					    if (ct.dev == -1 ||
						au_set_lr_value(sc, &ct, l, r))
						    goto usemaster;
					    else
						    break;
				    }
			}
		} else {
			ct.type = AUDIO_MIXER_SET;
			error = audio_get_port(sc, &ct);
			if (error)
				return error;
			mask = ct.un.mask;
			nset = 0;
			for(i = 0; i < ports->nports; i++) {
				if (ports->misel[i] & mask) {
				    ct.dev = ports->miport[i];
				    if (ct.dev != -1 &&
					au_set_lr_value(sc, &ct, l, r) == 0)
					    nset++;
				}
			}
			if (nset == 0)
				goto usemaster;
		}
	}
	if (!error)
		mixer_signal(sc);
	return error;
}

int
au_get_lr_value(struct	audio_softc *sc, mixer_ctrl_t *ct, int *l, int *r)
{
	int error;

	KASSERT(mutex_owned(sc->sc_lock));

	ct->un.value.num_channels = 2;
	if (audio_get_port(sc, ct) == 0) {
		*l = ct->un.value.level[AUDIO_MIXER_LEVEL_LEFT];
		*r = ct->un.value.level[AUDIO_MIXER_LEVEL_RIGHT];
	} else {
		ct->un.value.num_channels = 1;
		error = audio_get_port(sc, ct);
		if (error)
			return error;
		*r = *l = ct->un.value.level[AUDIO_MIXER_LEVEL_MONO];
	}
	return 0;
}

void
au_get_gain(struct audio_softc *sc, struct au_mixer_ports *ports,
	    u_int *pgain, u_char *pbalance)
{
	mixer_ctrl_t ct;
	int i, l, r, n;
	int lgain, rgain;

	KASSERT(mutex_owned(sc->sc_lock));

	lgain = AUDIO_MAX_GAIN / 2;
	rgain = AUDIO_MAX_GAIN / 2;
	if (ports->index == -1) {
	usemaster:
		if (ports->master == -1)
			goto bad;
		ct.dev = ports->master;
		ct.type = AUDIO_MIXER_VALUE;
		if (au_get_lr_value(sc, &ct, &lgain, &rgain))
			goto bad;
	} else {
		ct.dev = ports->index;
		if (ports->isenum) {
			ct.type = AUDIO_MIXER_ENUM;
			if (audio_get_port(sc, &ct))
				goto bad;
			ct.type = AUDIO_MIXER_VALUE;
			if (ports->isdual) {
				if (ports->cur_port == -1)
					ct.dev = ports->master;
				else
					ct.dev = ports->miport[ports->cur_port];
				au_get_lr_value(sc, &ct, &lgain, &rgain);
			} else {
				for(i = 0; i < ports->nports; i++)
				    if (ports->misel[i] == ct.un.ord) {
					    ct.dev = ports->miport[i];
					    if (ct.dev == -1 ||
						au_get_lr_value(sc, &ct,
								&lgain, &rgain))
						    goto usemaster;
					    else
						    break;
				    }
			}
		} else {
			ct.type = AUDIO_MIXER_SET;
			if (audio_get_port(sc, &ct))
				goto bad;
			ct.type = AUDIO_MIXER_VALUE;
			lgain = rgain = n = 0;
			for(i = 0; i < ports->nports; i++) {
				if (ports->misel[i] & ct.un.mask) {
					ct.dev = ports->miport[i];
					if (ct.dev == -1 ||
					    au_get_lr_value(sc, &ct, &l, &r))
						goto usemaster;
					else {
						lgain += l;
						rgain += r;
						n++;
					}
				}
			}
			if (n != 0) {
				lgain /= n;
				rgain /= n;
			}
		}
	}
bad:
	if (lgain == rgain) {	/* handles lgain==rgain==0 */
		*pgain = lgain;
		*pbalance = AUDIO_MID_BALANCE;
	} else if (lgain < rgain) {
		*pgain = rgain;
		/* balance should be > AUDIO_MID_BALANCE */
		*pbalance = AUDIO_RIGHT_BALANCE -
			(AUDIO_MID_BALANCE * lgain) / rgain;
	} else /* lgain > rgain */ {
		*pgain = lgain;
		/* balance should be < AUDIO_MID_BALANCE */
		*pbalance = (AUDIO_MID_BALANCE * rgain) / lgain;
	}
}

int
au_set_port(struct audio_softc *sc, struct au_mixer_ports *ports, u_int port)
{
	mixer_ctrl_t ct;
	int i, error, use_mixerout;

	KASSERT(mutex_owned(sc->sc_lock));

	use_mixerout = 1;
	if (port == 0) {
		if (ports->allports == 0)
			return 0;		/* Allow this special case. */
		else if (ports->isdual) {
			if (ports->cur_port == -1) {
				return 0;
			} else {
				port = ports->aumask[ports->cur_port];
				ports->cur_port = -1;
				use_mixerout = 0;
			}
		}
	}
	if (ports->index == -1)
		return EINVAL;
	ct.dev = ports->index;
	if (ports->isenum) {
		if (port & (port-1))
			return EINVAL; /* Only one port allowed */
		ct.type = AUDIO_MIXER_ENUM;
		error = EINVAL;
		for(i = 0; i < ports->nports; i++)
			if (ports->aumask[i] == port) {
				if (ports->isdual && use_mixerout) {
					ct.un.ord = ports->mixerout;
					ports->cur_port = i;
				} else {
					ct.un.ord = ports->misel[i];
				}
				error = audio_set_port(sc, &ct);
				break;
			}
	} else {
		ct.type = AUDIO_MIXER_SET;
		ct.un.mask = 0;
		for(i = 0; i < ports->nports; i++)
			if (ports->aumask[i] & port)
				ct.un.mask |= ports->misel[i];
		if (port != 0 && ct.un.mask == 0)
			error = EINVAL;
		else
			error = audio_set_port(sc, &ct);
	}
	if (!error)
		mixer_signal(sc);
	return error;
}

int
au_get_port(struct audio_softc *sc, struct au_mixer_ports *ports)
{
	mixer_ctrl_t ct;
	int i, aumask;

	KASSERT(mutex_owned(sc->sc_lock));

	if (ports->index == -1)
		return 0;
	ct.dev = ports->index;
	ct.type = ports->isenum ? AUDIO_MIXER_ENUM : AUDIO_MIXER_SET;
	if (audio_get_port(sc, &ct))
		return 0;
	aumask = 0;
	if (ports->isenum) {
		if (ports->isdual && ports->cur_port != -1) {
			if (ports->mixerout == ct.un.ord)
				aumask = ports->aumask[ports->cur_port];
			else
				ports->cur_port = -1;
		}
		if (aumask == 0)
			for(i = 0; i < ports->nports; i++)
				if (ports->misel[i] == ct.un.ord)
					aumask = ports->aumask[i];
	} else {
		for(i = 0; i < ports->nports; i++)
			if (ct.un.mask & ports->misel[i])
				aumask |= ports->aumask[i];
	}
	return aumask;
}

int
audiosetinfo(struct audio_softc *sc, struct audio_info *ai, bool reset, int n)
{
	stream_filter_list_t pfilters, rfilters;
	audio_params_t pp, rp;
	struct audio_prinfo *r, *p;
	const struct audio_hw_if *hw;
	struct virtual_channel *vc;
	audio_stream_t *oldpus, *oldrus;
	int setmode;
	int error;
	int np, nr;
	unsigned int blks;
	int oldpblksize, oldrblksize;
	u_int gain;
	bool rbus, pbus;
	bool cleared, modechange, pausechange;
	u_char balance;

	KASSERT(mutex_owned(sc->sc_lock));

	vc = sc->sc_vchan[n];
	hw = sc->hw_if;
	if (hw == NULL)		/* HW has not attached */
		return ENXIO;

	DPRINTF(("%s sc=%p ai=%p\n", __func__, sc, ai));
	r = &ai->record;
	p = &ai->play;
	rbus = vc->sc_rbus;
	pbus = vc->sc_pbus;
	error = 0;
	cleared = false;
	modechange = false;
	pausechange = false;

	pp = vc->sc_pparams;	/* Temporary encoding storage in */
	rp = vc->sc_rparams;	/* case setting the modes fails. */
	nr = np = 0;

	if (SPECIFIED(p->sample_rate)) {
		pp.sample_rate = p->sample_rate;
		np++;
	}
	if (SPECIFIED(r->sample_rate)) {
		rp.sample_rate = r->sample_rate;
		nr++;
	}
	if (SPECIFIED(p->encoding)) {
		pp.encoding = p->encoding;
		np++;
	}
	if (SPECIFIED(r->encoding)) {
		rp.encoding = r->encoding;
		nr++;
	}
	if (SPECIFIED(p->precision)) {
		pp.precision = p->precision;
		/* we don't have API to specify validbits */
		pp.validbits = p->precision;
		np++;
	}
	if (SPECIFIED(r->precision)) {
		rp.precision = r->precision;
		/* we don't have API to specify validbits */
		rp.validbits = r->precision;
		nr++;
	}
	if (SPECIFIED(p->channels)) {
		pp.channels = p->channels;
		np++;
	}
	if (SPECIFIED(r->channels)) {
		rp.channels = r->channels;
		nr++;
	}

	if (!audio_can_capture(sc))
		nr = 0;
	if (!audio_can_playback(sc))
		np = 0;

#ifdef AUDIO_DEBUG
	if (audiodebug && nr > 0)
	    audio_print_params("audiosetinfo() Setting record params:", &rp);
	if (audiodebug && np > 0)
	    audio_print_params("audiosetinfo() Setting play params:", &pp);
#endif
	if (nr > 0 && (error = audio_check_params(&rp)))
		return error;
	if (np > 0 && (error = audio_check_params(&pp)))
		return error;

	oldpblksize = vc->sc_mpr.blksize;
	oldrblksize = vc->sc_mrr.blksize;

	setmode = 0;
	if (nr > 0) {
		if (!cleared) {
			audio_clear_intr_unlocked(sc, n);
			cleared = true;
		}
		modechange = true;
		setmode |= AUMODE_RECORD;
	}
	if (np > 0) {
		if (!cleared) {
			audio_clear_intr_unlocked(sc, n);
			cleared = true;
		}
		modechange = true;
		setmode |= AUMODE_PLAY;
	}

	if (SPECIFIED(ai->mode)) {
		if (!cleared) {
			audio_clear_intr_unlocked(sc, n);
			cleared = true;
		}
		modechange = true;
		vc->sc_mode = ai->mode;
		if (vc->sc_mode & AUMODE_PLAY_ALL)
			vc->sc_mode |= AUMODE_PLAY;
		if ((vc->sc_mode & AUMODE_PLAY) && !vc->sc_full_duplex)
			/* Play takes precedence */
			vc->sc_mode &= ~AUMODE_RECORD;
	}

	oldpus = vc->sc_pustream;
	oldrus = vc->sc_rustream;
	if (modechange || reset) {
		int indep;

		indep = audio_get_props(sc) & AUDIO_PROP_INDEPENDENT;
		if (!indep) {
			if (setmode == AUMODE_RECORD)
				pp = rp;
			else if (setmode == AUMODE_PLAY)
				rp = pp;
		}
		memset(&pfilters, 0, sizeof(pfilters));
		memset(&rfilters, 0, sizeof(rfilters));
		pfilters.append = stream_filter_list_append;
		pfilters.prepend = stream_filter_list_prepend;
		pfilters.set = stream_filter_list_set;
		rfilters.append = stream_filter_list_append;
		rfilters.prepend = stream_filter_list_prepend;
		rfilters.set = stream_filter_list_set;
		/* Some device drivers change channels/sample_rate and change
		 * no channels/sample_rate. */
		error = audio_set_params(sc, setmode,
		    vc->sc_mode & (AUMODE_PLAY | AUMODE_RECORD), &pp, &rp,
		    &pfilters, &rfilters, n);
		if (error) {
			DPRINTF(("%s: audio_set_params() failed with %d\n",
			    __func__, error));
			goto cleanup;
		}

		audio_check_params(&pp);
		audio_check_params(&rp);
		if (!indep) {
			/* XXX for !indep device, we have to use the same
			 * parameters for the hardware, not userland */
			if (setmode == AUMODE_RECORD) {
				pp = rp;
			} else if (setmode == AUMODE_PLAY) {
				rp = pp;
			}
		}

		if (vc->sc_mpr.mmapped && pfilters.req_size > 0) {
			DPRINTF(("%s: mmapped, and filters are requested.\n",
				 __func__));
			error = EINVAL;
			goto cleanup;
		}

		/* construct new filter chain */
		if (setmode & AUMODE_PLAY) {
			error = audio_setup_pfilters(sc, &pp, &pfilters, n);
			if (error)
				goto cleanup;
		}
		if (setmode & AUMODE_RECORD) {
			error = audio_setup_rfilters(sc, &rp, &rfilters, n);
			if (error)
				goto cleanup;
		}
		DPRINTF(("%s: filter setup is completed.\n", __func__));

		/* userland formats */
		vc->sc_pparams = pp;
		vc->sc_rparams = rp;
	}

	/* Play params can affect the record params, so recalculate blksize. */
	if (nr > 0 || np > 0 || reset) {
		vc->sc_blkset = false;
		audio_calc_blksize(sc, AUMODE_RECORD, n);
		audio_calc_blksize(sc, AUMODE_PLAY, n);
	}
#ifdef AUDIO_DEBUG
	if (audiodebug > 1 && nr > 0) {
	    audio_print_params("audiosetinfo() After setting record params:",
		&vc->sc_rparams);
	}
	if (audiodebug > 1 && np > 0) {
	    audio_print_params("audiosetinfo() After setting play params:",
		&vc->sc_pparams);
	}
#endif

	if (SPECIFIED(p->port)) {
		if (!cleared) {
			audio_clear_intr_unlocked(sc, n);
			cleared = true;
		}
		error = au_set_port(sc, &sc->sc_outports, p->port);
		if (error)
			goto cleanup;
	}
	if (SPECIFIED(r->port)) {
		if (!cleared) {
			audio_clear_intr_unlocked(sc, n);
			cleared = true;
		}
		error = au_set_port(sc, &sc->sc_inports, r->port);
		if (error)
			goto cleanup;
	}
	if (SPECIFIED(p->gain)) {
		au_get_gain(sc, &sc->sc_outports, &gain, &balance);
		error = au_set_gain(sc, &sc->sc_outports, p->gain, balance);
		if (error)
			goto cleanup;
	}
	if (SPECIFIED(r->gain)) {
		au_get_gain(sc, &sc->sc_inports, &gain, &balance);
		error = au_set_gain(sc, &sc->sc_inports, r->gain, balance);
		if (error)
			goto cleanup;
	}

	if (SPECIFIED_CH(p->balance)) {
		au_get_gain(sc, &sc->sc_outports, &gain, &balance);
		error = au_set_gain(sc, &sc->sc_outports, gain, p->balance);
		if (error)
			goto cleanup;
	}
	if (SPECIFIED_CH(r->balance)) {
		au_get_gain(sc, &sc->sc_inports, &gain, &balance);
		error = au_set_gain(sc, &sc->sc_inports, gain, r->balance);
		if (error)
			goto cleanup;
	}

	if (SPECIFIED(ai->monitor_gain) && sc->sc_monitor_port != -1) {
		mixer_ctrl_t ct;

		ct.dev = sc->sc_monitor_port;
		ct.type = AUDIO_MIXER_VALUE;
		ct.un.value.num_channels = 1;
		ct.un.value.level[AUDIO_MIXER_LEVEL_MONO] = ai->monitor_gain;
		error = audio_set_port(sc, &ct);
		if (error)
			goto cleanup;
	}

	if (SPECIFIED_CH(p->pause)) {
		vc->sc_mpr.pause = p->pause;
		pbus = !p->pause;
		pausechange = true;
	}
	if (SPECIFIED_CH(r->pause)) {
		vc->sc_mrr.pause = r->pause;
		rbus = !r->pause;
		pausechange = true;
	}

	if (SPECIFIED(ai->blocksize)) {
		int pblksize, rblksize;

		/* Block size specified explicitly. */
		if (ai->blocksize == 0) {
			if (!cleared) {
				audio_clear_intr_unlocked(sc, n);
				cleared = true;
			}
			vc->sc_blkset = false;
			audio_calc_blksize(sc, AUMODE_RECORD, n);
			audio_calc_blksize(sc, AUMODE_PLAY, n);
		} else {
			vc->sc_blkset = true;
			/* check whether new blocksize changes actually */
			if (hw->round_blocksize == NULL) {
				if (!cleared) {
					audio_clear_intr_unlocked(sc, n);
					cleared = true;
				}
				vc->sc_mpr.blksize = ai->blocksize;
				vc->sc_mrr.blksize = ai->blocksize;
			} else {
				pblksize = hw->round_blocksize(sc->hw_hdl,
				    ai->blocksize, AUMODE_PLAY,
				    &vc->sc_mpr.s.param);
				rblksize = hw->round_blocksize(sc->hw_hdl,
				    ai->blocksize, AUMODE_RECORD,
				    &vc->sc_mrr.s.param);
				if ((pblksize != vc->sc_mpr.blksize &&
				    pblksize > sc->sc_vchan[0]->sc_mpr.blksize)
				    || (rblksize != vc->sc_mrr.blksize &&
				    rblksize >
					sc->sc_vchan[0]->sc_mrr.blksize)) {
					if (!cleared) {
					    audio_clear_intr_unlocked(sc, n);
					    cleared = true;
					}
					vc->sc_mpr.blksize = pblksize;
					vc->sc_mrr.blksize = rblksize;
				}
			}
		}
	}

	if (SPECIFIED(ai->mode)) {
		if (vc->sc_mode & AUMODE_PLAY)
			audio_init_play(sc, n);
		if (vc->sc_mode & AUMODE_RECORD)
			audio_init_record(sc, n);
	}

	if (hw->commit_settings && sc->sc_opens == 0) {
		error = hw->commit_settings(sc->hw_hdl);
		if (error)
			goto cleanup;
	}

	vc->sc_lastinfo = *ai;
	vc->sc_lastinfovalid = true;

cleanup:
	if (error == 0 && (cleared || pausechange|| reset)) {
		int init_error;

		mutex_enter(sc->sc_intr_lock);
		init_error = (pausechange == 1 && reset == 0) ? 0 :
		    audio_initbufs(sc, n);
		if (init_error) goto err;
		if (vc->sc_mpr.blksize != oldpblksize ||
		    vc->sc_mrr.blksize != oldrblksize ||
		    vc->sc_pustream != oldpus ||
		    vc->sc_rustream != oldrus)
			audio_calcwater(sc, n);
		if ((vc->sc_mode & AUMODE_PLAY) &&
		    pbus && !vc->sc_pbus)
			init_error = audiostartp(sc, n);
		if (!init_error &&
		    (vc->sc_mode & AUMODE_RECORD) &&
		    rbus && !vc->sc_rbus)
			init_error = audiostartr(sc, n);
	err:
		mutex_exit(sc->sc_intr_lock);
		if (init_error)
			return init_error;
	}

	/* Change water marks after initializing the buffers. */
	if (SPECIFIED(ai->hiwat)) {
		blks = ai->hiwat;
		if (blks > vc->sc_mpr.maxblks)
			blks = vc->sc_mpr.maxblks;
		if (blks < 2)
			blks = 2;
		vc->sc_mpr.usedhigh = blks * vc->sc_mpr.blksize;
	}
	if (SPECIFIED(ai->lowat)) {
		blks = ai->lowat;
		if (blks > vc->sc_mpr.maxblks - 1)
			blks = vc->sc_mpr.maxblks - 1;
		vc->sc_mpr.usedlow = blks * vc->sc_mpr.blksize;
	}
	if (SPECIFIED(ai->hiwat) || SPECIFIED(ai->lowat)) {
		if (vc->sc_mpr.usedlow > vc->sc_mpr.usedhigh -
		    vc->sc_mpr.blksize) {
			vc->sc_mpr.usedlow =
				vc->sc_mpr.usedhigh - vc->sc_mpr.blksize;
		}
	}

	return error;
}

int
audiogetinfo(struct audio_softc *sc, struct audio_info *ai, int buf_only_mode,
    int n)
{
	struct audio_prinfo *r, *p;
	const struct audio_hw_if *hw;
	struct virtual_channel *vc;

	KASSERT(mutex_owned(sc->sc_lock));

	r = &ai->record;
	p = &ai->play;
	vc = sc->sc_vchan[n];
	hw = sc->hw_if;
	if (hw == NULL)		/* HW has not attached */
		return ENXIO;

	p->sample_rate = vc->sc_pparams.sample_rate;
	r->sample_rate = vc->sc_rparams.sample_rate;
	p->channels = vc->sc_pparams.channels;
	r->channels = vc->sc_rparams.channels;
	p->precision = vc->sc_pparams.precision;
	r->precision = vc->sc_rparams.precision;
	p->encoding = vc->sc_pparams.encoding;
	r->encoding = vc->sc_rparams.encoding;

	if (buf_only_mode) {
		r->port = 0;
		p->port = 0;

		r->avail_ports = 0;
		p->avail_ports = 0;

		r->gain = 0;
		r->balance = 0;

		p->gain = 0;
		p->balance = 0;
	} else {
		r->port = au_get_port(sc, &sc->sc_inports);
		p->port = au_get_port(sc, &sc->sc_outports);

		r->avail_ports = sc->sc_inports.allports;
		p->avail_ports = sc->sc_outports.allports;

		au_get_gain(sc, &sc->sc_inports, &r->gain, &r->balance);
		au_get_gain(sc, &sc->sc_outports, &p->gain, &p->balance);
	}

	if (sc->sc_monitor_port != -1 && buf_only_mode == 0) {
		mixer_ctrl_t ct;

		ct.dev = sc->sc_monitor_port;
		ct.type = AUDIO_MIXER_VALUE;
		ct.un.value.num_channels = 1;
		if (audio_get_port(sc, &ct))
			ai->monitor_gain = 0;
		else
			ai->monitor_gain =
				ct.un.value.level[AUDIO_MIXER_LEVEL_MONO];
	} else
		ai->monitor_gain = 0;

	p->seek = audio_stream_get_used(vc->sc_pustream);
	r->seek = audio_stream_get_used(vc->sc_rustream);

	/*
	 * XXX samples should be a value for userland data.
	 * But drops is a value for HW data.
	 */
	p->samples = (vc->sc_pustream == &vc->sc_mpr.s
	    ? vc->sc_mpr.stamp : vc->sc_mpr.fstamp) - vc->sc_mpr.drops;
	r->samples = (vc->sc_rustream == &vc->sc_mrr.s
	    ? vc->sc_mrr.stamp : vc->sc_mrr.fstamp) - vc->sc_mrr.drops;

	p->eof = sc->sc_eof;
	r->eof = 0;

	p->pause = vc->sc_mpr.pause;
	r->pause = vc->sc_mrr.pause;

	p->error = vc->sc_mpr.drops != 0;
	r->error = vc->sc_mrr.drops != 0;

	p->waiting = r->waiting = 0;		/* open never hangs */

	p->open = (vc->sc_open & AUOPEN_WRITE) != 0;
	r->open = (vc->sc_open & AUOPEN_READ) != 0;

	p->active = vc->sc_pbus;
	r->active = vc->sc_rbus;

	p->buffer_size = vc->sc_pustream ? vc->sc_pustream->bufsize : 0;
	r->buffer_size = vc->sc_rustream ? vc->sc_rustream->bufsize : 0;

	ai->blocksize = vc->sc_mpr.blksize;
	if (vc->sc_mpr.blksize > 0) {
		ai->hiwat = vc->sc_mpr.usedhigh / vc->sc_mpr.blksize;
		ai->lowat = vc->sc_mpr.usedlow / vc->sc_mpr.blksize;
	} else
		ai->hiwat = ai->lowat = 0;
	ai->mode = vc->sc_mode;

	return 0;
}

/*
 * Mixer driver
 */
int
mixer_open(dev_t dev, struct audio_softc *sc, int flags,
    int ifmt, struct lwp *l)
{

	KASSERT(mutex_owned(sc->sc_lock));

	if (sc->hw_if == NULL)
		return  ENXIO;

	DPRINTF(("mixer_open: flags=0x%x sc=%p\n", flags, sc));

	return 0;
}

/*
 * Remove a process from those to be signalled on mixer activity.
 */
static void
mixer_remove(struct audio_softc *sc)
{
	struct mixer_asyncs **pm, *m;
	pid_t pid;

	KASSERT(mutex_owned(sc->sc_lock));

	pid = curproc->p_pid;
	for (pm = &sc->sc_async_mixer; *pm; pm = &(*pm)->next) {
		if ((*pm)->pid == pid) {
			m = *pm;
			*pm = m->next;
			kmem_free(m, sizeof(*m));
			return;
		}
	}
}

/*
 * Signal all processes waiting for the mixer.
 */
static void
mixer_signal(struct audio_softc *sc)
{
	struct mixer_asyncs *m;
	proc_t *p;

	for (m = sc->sc_async_mixer; m; m = m->next) {
		mutex_enter(proc_lock);
		if ((p = proc_find(m->pid)) != NULL)
			psignal(p, SIGIO);
		mutex_exit(proc_lock);
	}
}

/*
 * Close a mixer device
 */
/* ARGSUSED */
int
mixer_close(struct audio_softc *sc, int flags, int ifmt, struct lwp *l)
{

	KASSERT(mutex_owned(sc->sc_lock));
	if (sc->hw_if == NULL)
		return ENXIO;

	DPRINTF(("mixer_close: sc %p\n", sc));
	mixer_remove(sc);
	return 0;
}

int
mixer_ioctl(struct audio_softc *sc, u_long cmd, void *addr, int flag,
	    struct lwp *l)
{
	const struct audio_hw_if *hw;
	struct mixer_asyncs *ma;
	mixer_ctrl_t *mc;
	int error;

	DPRINTF(("mixer_ioctl(%lu,'%c',%lu)\n",
		 IOCPARM_LEN(cmd), (char)IOCGROUP(cmd), cmd&0xff));
	hw = sc->hw_if;
	if (hw == NULL)
		return ENXIO;
	error = EINVAL;

	/* we can return cached values if we are sleeping */
	if (cmd != AUDIO_MIXER_READ)
		device_active(sc->dev, DVA_SYSTEM);

	switch (cmd) {
	case FIOASYNC:
		if (*(int *)addr) {
			mutex_exit(sc->sc_lock);
			ma = kmem_alloc(sizeof(struct mixer_asyncs), KM_SLEEP);
			mutex_enter(sc->sc_lock);
		} else {
			ma = NULL;
		}
		mixer_remove(sc);	/* remove old entry */
		if (ma != NULL) {
			ma->next = sc->sc_async_mixer;
			ma->pid = curproc->p_pid;
			sc->sc_async_mixer = ma;
		}
		error = 0;
		break;

	case AUDIO_GETDEV:
		DPRINTF(("AUDIO_GETDEV\n"));
		error = hw->getdev(sc->hw_hdl, (audio_device_t *)addr);
		break;

	case AUDIO_MIXER_DEVINFO:
		DPRINTF(("AUDIO_MIXER_DEVINFO\n"));
		((mixer_devinfo_t *)addr)->un.v.delta = 0; /* default */
		error = audio_query_devinfo(sc, (mixer_devinfo_t *)addr);
		break;

	case AUDIO_MIXER_READ:
		DPRINTF(("AUDIO_MIXER_READ\n"));
		mc = (mixer_ctrl_t *)addr;

		if (device_is_active(sc->sc_dev))
			error = audio_get_port(sc, mc);
		else if (mc->dev >= sc->sc_nmixer_states)
			error = ENXIO;
		else {
			int dev = mc->dev;
			memcpy(mc, &sc->sc_mixer_state[dev],
			    sizeof(mixer_ctrl_t));
			error = 0;
		}
		break;

	case AUDIO_MIXER_WRITE:
		DPRINTF(("AUDIO_MIXER_WRITE\n"));
		error = audio_set_port(sc, (mixer_ctrl_t *)addr);
		if (!error && hw->commit_settings)
			error = hw->commit_settings(sc->hw_hdl);
		if (!error)
			mixer_signal(sc);
		break;

	default:
		if (hw->dev_ioctl)
			error = hw->dev_ioctl(sc->hw_hdl, cmd, addr, flag, l);
		else
			error = EINVAL;
		break;
	}
	DPRINTF(("mixer_ioctl(%lu,'%c',%lu) result %d\n",
		 IOCPARM_LEN(cmd), (char)IOCGROUP(cmd), cmd&0xff, error));
	return error;
}
#endif /* NAUDIO > 0 */

#include "midi.h"

#if NAUDIO == 0 && (NMIDI > 0 || NMIDIBUS > 0)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/audioio.h>
#include <dev/audio_if.h>
#endif

#if NAUDIO > 0 || (NMIDI > 0 || NMIDIBUS > 0)
int
audioprint(void *aux, const char *pnp)
{
	struct audio_attach_args *arg;
	const char *type;

	if (pnp != NULL) {
		arg = aux;
		switch (arg->type) {
		case AUDIODEV_TYPE_AUDIO:
			type = "audio";
			break;
		case AUDIODEV_TYPE_MIDI:
			type = "midi";
			break;
		case AUDIODEV_TYPE_OPL:
			type = "opl";
			break;
		case AUDIODEV_TYPE_MPU:
			type = "mpu";
			break;
		default:
			panic("audioprint: unknown type %d", arg->type);
		}
		aprint_normal("%s at %s", type, pnp);
	}
	return UNCONF;
}

#endif /* NAUDIO > 0 || (NMIDI > 0 || NMIDIBUS > 0) */

#if NAUDIO > 0
device_t
audio_get_device(struct audio_softc *sc)
{
	return sc->sc_dev;
}
#endif

#if NAUDIO > 0
static void
audio_mixer_capture(struct audio_softc *sc)
{
	mixer_devinfo_t mi;
	mixer_ctrl_t *mc;

	KASSERT(mutex_owned(sc->sc_lock));

	for (mi.index = 0;; mi.index++) {
		if (audio_query_devinfo(sc, &mi) != 0)
			break;
		KASSERT(mi.index < sc->sc_nmixer_states);
		if (mi.type == AUDIO_MIXER_CLASS)
			continue;
		mc = &sc->sc_mixer_state[mi.index];
		mc->dev = mi.index;
		mc->type = mi.type;
		mc->un.value.num_channels = mi.un.v.num_channels;
		(void)audio_get_port(sc, mc);
	}

	return;
}

static void
audio_mixer_restore(struct audio_softc *sc)
{
	mixer_devinfo_t mi;
	mixer_ctrl_t *mc;

	KASSERT(mutex_owned(sc->sc_lock));

	for (mi.index = 0; ; mi.index++) {
		if (audio_query_devinfo(sc, &mi) != 0)
			break;
		if (mi.type == AUDIO_MIXER_CLASS)
			continue;
		mc = &sc->sc_mixer_state[mi.index];
		(void)audio_set_port(sc, mc);
	}
	if (sc->hw_if->commit_settings)
		sc->hw_if->commit_settings(sc->hw_hdl);

	return;
}

#ifdef AUDIO_PM_IDLE
static void
audio_idle(void *arg)
{
	device_t dv = arg;
	struct audio_softc *sc = device_private(dv);

#ifdef PNP_DEBUG
	extern int pnp_debug_idle;
	if (pnp_debug_idle)
		printf("%s: idle handler called\n", device_xname(dv));
#endif

	sc->sc_idle = true;

	/* XXX joerg Make pmf_device_suspend handle children? */
	if (!pmf_device_suspend(dv, PMF_Q_SELF))
		return;

	if (!pmf_device_suspend(sc->sc_dev, PMF_Q_SELF))
		pmf_device_resume(dv, PMF_Q_SELF);
}

static void
audio_activity(device_t dv, devactive_t type)
{
	struct audio_softc *sc = device_private(dv);

	if (type != DVA_SYSTEM)
		return;

	callout_schedule(&sc->sc_idle_counter, audio_idle_timeout * hz);

	sc->sc_idle = false;
	if (!device_is_active(dv)) {
		/* XXX joerg How to deal with a failing resume... */
		pmf_device_resume(sc->sc_dev, PMF_Q_SELF);
		pmf_device_resume(dv, PMF_Q_SELF);
	}
}
#endif

static bool
audio_suspend(device_t dv, const pmf_qual_t *qual)
{
	struct audio_softc *sc = device_private(dv);
	const struct audio_hw_if *hwp = sc->hw_if;
	struct virtual_channel *vc;
	int n;
	bool pbus, rbus;

	pbus = rbus = false;
	mutex_enter(sc->sc_lock);
	audio_mixer_capture(sc);
	for (n = 1; n < VAUDIOCHANS; n++) {
		if (sc->sc_audiopid[n].pid == -1)
			continue;

		vc = sc->sc_vchan[n];
		if (vc->sc_pbus && !pbus)
			pbus = true;
		if (vc->sc_rbus && !rbus)
			rbus = true;
	}
	mutex_enter(sc->sc_intr_lock);
	if (pbus == true)
		hwp->halt_output(sc->hw_hdl);
	if (rbus == true)
		hwp->halt_input(sc->hw_hdl);
	mutex_exit(sc->sc_intr_lock);
#ifdef AUDIO_PM_IDLE
	callout_halt(&sc->sc_idle_counter, sc->sc_lock);
#endif
	mutex_exit(sc->sc_lock);

	return true;
}

static bool
audio_resume(device_t dv, const pmf_qual_t *qual)
{
	struct audio_softc *sc = device_private(dv);
	struct virtual_channel *vc;
	int n;
	
	mutex_enter(sc->sc_lock);
	sc->sc_trigger_started = false;
	sc->sc_rec_started = false;

	audio_set_vchan_defaults(sc, AUMODE_PLAY | AUMODE_PLAY_ALL |
	    AUMODE_RECORD, &sc->sc_format[0], 0);

	audio_mixer_restore(sc);
	for (n = 1; n < VAUDIOCHANS; n++) {
		if (sc->sc_audiopid[n].pid == -1)
			continue;
		vc = sc->sc_vchan[n];

		if (vc->sc_lastinfovalid == true)
			audiosetinfo(sc, &vc->sc_lastinfo, true, n);
		mutex_enter(sc->sc_intr_lock);
		if (vc->sc_pbus == true && !vc->sc_mpr.pause)
			audiostartp(sc, n);
		if (vc->sc_rbus == true && !vc->sc_mrr.pause)
			audiostartr(sc, n);
		mutex_exit(sc->sc_intr_lock);
	}
	mutex_exit(sc->sc_lock);

	return true;
}

static void
audio_volume_down(device_t dv)
{
	struct audio_softc *sc = device_private(dv);
	mixer_devinfo_t mi;
	int newgain;
	u_int gain;
	u_char balance;

	mutex_enter(sc->sc_lock);
	if (sc->sc_outports.index == -1 && sc->sc_outports.master != -1) {
		mi.index = sc->sc_outports.master;
		mi.un.v.delta = 0;
		if (audio_query_devinfo(sc, &mi) == 0) {
			au_get_gain(sc, &sc->sc_outports, &gain, &balance);
			newgain = gain - mi.un.v.delta;
			if (newgain < AUDIO_MIN_GAIN)
				newgain = AUDIO_MIN_GAIN;
			au_set_gain(sc, &sc->sc_outports, newgain, balance);
		}
	}
	mutex_exit(sc->sc_lock);
}

static void
audio_volume_up(device_t dv)
{
	struct audio_softc *sc = device_private(dv);
	mixer_devinfo_t mi;
	u_int gain, newgain;
	u_char balance;

	mutex_enter(sc->sc_lock);
	if (sc->sc_outports.index == -1 && sc->sc_outports.master != -1) {
		mi.index = sc->sc_outports.master;
		mi.un.v.delta = 0;
		if (audio_query_devinfo(sc, &mi) == 0) {
			au_get_gain(sc, &sc->sc_outports, &gain, &balance);
			newgain = gain + mi.un.v.delta;
			if (newgain > AUDIO_MAX_GAIN)
				newgain = AUDIO_MAX_GAIN;
			au_set_gain(sc, &sc->sc_outports, newgain, balance);
		}
	}
	mutex_exit(sc->sc_lock);
}

static void
audio_volume_toggle(device_t dv)
{
	struct audio_softc *sc = device_private(dv);
	u_int gain, newgain;
	u_char balance;

	mutex_enter(sc->sc_lock);
	au_get_gain(sc, &sc->sc_outports, &gain, &balance);
	if (gain != 0) {
		sc->sc_lastgain = gain;
		newgain = 0;
	} else
		newgain = sc->sc_lastgain;
	au_set_gain(sc, &sc->sc_outports, newgain, balance);
	mutex_exit(sc->sc_lock);
}

static int
audio_get_props(struct audio_softc *sc)
{
	const struct audio_hw_if *hw;
	int props;

	KASSERT(mutex_owned(sc->sc_lock));

	hw = sc->hw_if;
	props = hw->get_props(sc->hw_hdl);

	/*
	 * if neither playback nor capture properties are reported,
	 * assume both are supported by the device driver
	 */
	if ((props & (AUDIO_PROP_PLAYBACK|AUDIO_PROP_CAPTURE)) == 0)
		props |= (AUDIO_PROP_PLAYBACK | AUDIO_PROP_CAPTURE);

	return props;
}

static bool
audio_can_playback(struct audio_softc *sc)
{
	return audio_get_props(sc) & AUDIO_PROP_PLAYBACK ? true : false;
}

static bool
audio_can_capture(struct audio_softc *sc)
{
	return audio_get_props(sc) & AUDIO_PROP_CAPTURE ? true : false;
}

void
mix_read(void *arg)
{
	struct audio_softc *sc = arg;
	struct virtual_channel *vc;
	stream_filter_t *filter;
	stream_fetcher_t *fetcher;
	stream_fetcher_t null_fetcher;
	int cc, cc1, blksize, error;
	uint8_t *inp;

	vc = sc->sc_vchan[0];
	blksize = vc->sc_mrr.blksize;
	cc = blksize;

	if (sc->hw_if->trigger_input && sc->sc_rec_started == false) {
		DPRINTF(("%s: call trigger_input\n", __func__));
		error = sc->hw_if->trigger_input(sc->hw_hdl, vc->sc_mrr.s.start,
		    vc->sc_mrr.s.end, blksize,
		    audio_rint, (void *)sc, &vc->sc_mrr.s.param);
	} else if (sc->hw_if->start_input) {
		DPRINTF(("%s: call start_input\n", __func__));
		error = sc->hw_if->start_input(sc->hw_hdl,
		    vc->sc_mrr.s.inp, blksize,
		    audio_rint, (void *)sc);
		if (error) {
			/* XXX does this really help? */
			DPRINTF(("audio_upmix restart failed: %d\n", error));
			audio_clear(sc, 0);
		}
	}
	sc->sc_rec_started = true;

	inp = vc->sc_mrr.s.inp;
	vc->sc_mrr.s.inp = audio_stream_add_inp(&vc->sc_mrr.s, inp, cc);

	if (vc->sc_nrfilters > 0) {
		cc = vc->sc_rustream->end - vc->sc_rustream->start;
		null_fetcher.fetch_to = null_fetcher_fetch_to;
		filter = vc->sc_rfilters[0];
		filter->set_fetcher(filter, &null_fetcher);
		fetcher = &vc->sc_rfilters[vc->sc_nrfilters - 1]->base;
		fetcher->fetch_to(sc, fetcher, vc->sc_rustream, cc);
	}

	blksize = audio_stream_get_used(vc->sc_rustream);
	cc1 = blksize;
	if (vc->sc_rustream->outp + blksize > vc->sc_rustream->end)
		cc1 = vc->sc_rustream->end - vc->sc_rustream->outp;
	memcpy(sc->sc_rr.s.start, vc->sc_rustream->outp, cc1);
	if (cc1 < blksize) {
		memcpy(sc->sc_rr.s.start + cc1, vc->sc_rustream->start,
		    blksize - cc1);
	}
	sc->sc_rr.s.inp = audio_stream_add_inp(&sc->sc_rr.s, sc->sc_rr.s.inp,
	    blksize);
	vc->sc_rustream->outp = audio_stream_add_outp(vc->sc_rustream,
	    vc->sc_rustream->outp, blksize);
}

void
mix_write(void *arg)
{
	struct audio_softc *sc = arg;
	struct virtual_channel *vc;
	stream_filter_t *filter;
	stream_fetcher_t *fetcher;
	stream_fetcher_t null_fetcher;
	int cc, cc1, cc2, blksize, error, used;
	uint8_t *inp, *orig, *tocopy;

	vc = sc->sc_vchan[0];
	blksize = vc->sc_mpr.blksize;
	cc = blksize;
	error = 0;

	tocopy = vc->sc_pustream->inp;
	orig = __UNCONST(sc->sc_pr.s.outp);
	used = blksize;
	while (used > 0) {
		cc = used;
		cc1 = vc->sc_pustream->end - tocopy;
		cc2 = sc->sc_pr.s.end - orig;
		if (cc2 < cc1)
			cc = cc2;
		else
			cc = cc1;
		if (cc > used)
			cc = used;
		memcpy(tocopy, orig, cc);
		orig += cc;
		tocopy += cc;

		if (tocopy >= vc->sc_pustream->end)
			tocopy = vc->sc_pustream->start;
		if (orig >= sc->sc_pr.s.end)
			orig = sc->sc_pr.s.start;

		used -= cc;
 	}

	inp = vc->sc_pustream->inp;
	vc->sc_pustream->inp = audio_stream_add_inp(vc->sc_pustream,
	    inp, blksize);

	cc = blksize;
	cc2 = sc->sc_pr.s.end - sc->sc_pr.s.inp;
	if (cc2 < cc) {
		memset(sc->sc_pr.s.inp, 0, cc2);
		cc -= cc2;
		memset(sc->sc_pr.s.start, 0, cc);
	} else
		memset(sc->sc_pr.s.inp, 0, cc);

	sc->sc_pr.s.outp = audio_stream_add_outp(&sc->sc_pr.s,
	    sc->sc_pr.s.outp, blksize);

	if (vc->sc_npfilters > 0) {
		null_fetcher.fetch_to = null_fetcher_fetch_to;
		filter = vc->sc_pfilters[0];
		filter->set_fetcher(filter, &null_fetcher);
		fetcher = &vc->sc_pfilters[vc->sc_npfilters - 1]->base;
		fetcher->fetch_to(sc, fetcher, &vc->sc_mpr.s, blksize);
 	}

	if (sc->hw_if->trigger_output && sc->sc_trigger_started == false) {
		DPRINTF(("%s: call trigger_output\n", __func__));
		error = sc->hw_if->trigger_output(sc->hw_hdl,
		    vc->sc_mpr.s.start, vc->sc_mpr.s.end, blksize,
		    audio_pint, (void *)sc, &vc->sc_mpr.s.param);
	} else if (sc->hw_if->start_output) {
		DPRINTF(("%s: call start_output\n", __func__));
		error = sc->hw_if->start_output(sc->hw_hdl,
		    __UNCONST(vc->sc_mpr.s.outp), blksize,
		    audio_pint, (void *)sc);
	}
	sc->sc_trigger_started = true;

	if (error) {
		/* XXX does this really help? */
		DPRINTF(("audio_mix restart failed: %d\n", error));
		audio_clear(sc, 0);
		sc->sc_trigger_started = false;
	}
}

void
mix_func8(struct audio_softc *sc, struct audio_ringbuffer *cb, int n)
{
	int blksize, cc, cc1, cc2, m, resid;
	int8_t *orig, *tomix;

	blksize = sc->sc_vchan[0]->sc_mpr.blksize;
	resid = blksize;

	tomix = __UNCONST(cb->s.outp);
	orig = (int8_t *)(sc->sc_pr.s.inp);

	while (resid > 0) {
		cc = resid;
		cc1 = sc->sc_pr.s.end - (uint8_t *)orig;
		cc2 = cb->s.end - (uint8_t *)tomix;
		if (cc > cc1)
			cc = cc1;
		if (cc > cc2)
			cc = cc2;

		for (m = 0; m < cc;  m++) {
			orig[m] += (int8_t)((int32_t)(tomix[m] *
			    ((sc->sc_vchan[n]->sc_swvol + 1) * 16)) /
			    (sc->sc_opens * VAUDIOCHANS));
		}

		if (&orig[m] >= (int8_t *)sc->sc_pr.s.end)
			orig = (int8_t *)sc->sc_pr.s.start;
		if (&tomix[m] >= (int8_t *)cb->s.end)
			tomix = (int8_t *)cb->s.start;

		resid -= cc;
	}
}

void
mix_func16(struct audio_softc *sc, struct audio_ringbuffer *cb, int n)
{
	int blksize, cc, cc1, cc2, m, resid;
	int16_t *orig, *tomix;

	blksize = sc->sc_vchan[0]->sc_mpr.blksize;
	resid = blksize;

	tomix = __UNCONST(cb->s.outp);
	orig = (int16_t *)(sc->sc_pr.s.inp);

	while (resid > 0) {
		cc = resid;
		cc1 = sc->sc_pr.s.end - (uint8_t *)orig;
		cc2 = cb->s.end - (uint8_t *)tomix;
		if (cc > cc1)
			cc = cc1;
		if (cc > cc2)
			cc = cc2;

		for (m = 0; m < cc / 2; m++) {
			orig[m] += (int16_t)((int32_t)(tomix[m] *
			    ((sc->sc_vchan[n]->sc_swvol + 1) * 16)) /
			    (sc->sc_opens * VAUDIOCHANS));
		}

		if (&orig[m] >= (int16_t *)sc->sc_pr.s.end)
			orig = (int16_t *)sc->sc_pr.s.start;
		if (&tomix[m] >= (int16_t *)cb->s.end)
			tomix = (int16_t *)cb->s.start;

		resid -= cc;
	}
}

void
mix_func32(struct audio_softc *sc, struct audio_ringbuffer *cb, int n)
{
	int blksize, cc, cc1, cc2, m, resid;
	int32_t *orig, *tomix;

	blksize = sc->sc_vchan[0]->sc_mpr.blksize;
	resid = blksize;

	tomix = __UNCONST(cb->s.outp);
	orig = (int32_t *)(sc->sc_pr.s.inp);

	while (resid > 0) {
		cc = resid;
		cc1 = sc->sc_pr.s.end - (uint8_t *)orig;
		cc2 = cb->s.end - (uint8_t *)tomix;
		if (cc > cc1)
			cc = cc1;
		if (cc > cc2)
			cc = cc2;

		for (m = 0; m < cc / 4; m++) {
			orig[m] += (int32_t)((int32_t)(tomix[m] *
			    ((sc->sc_vchan[n]->sc_swvol + 1) * 16)) /
			    (sc->sc_opens * VAUDIOCHANS));
		}

		if (&orig[m] >= (int32_t *)sc->sc_pr.s.end)
			orig = (int32_t *)sc->sc_pr.s.start;
		if (&tomix[m] >= (int32_t *)cb->s.end)
			tomix = (int32_t *)cb->s.start;

		resid -= cc;
	}
}

void
mix_func(struct audio_softc *sc, struct audio_ringbuffer *cb, int n)
{
	switch (sc->sc_precision) {
	case 8:
		mix_func8(sc, cb, n);
		break;
	case 16:
		mix_func16(sc, cb, n);
		break;
	case 24:
	case 32:
		mix_func32(sc, cb, n);
		break;
	default:
		break;
	}
}

void
saturate_func8(struct audio_softc *sc)
{
	int blksize, m, i, resid;
	int8_t *orig;

	blksize = sc->sc_vchan[0]->sc_mpr.blksize;
	resid = blksize;
	if (sc->sc_trigger_started == false)
		resid *= 2;

	orig = (int8_t *)(sc->sc_pr.s.inp);

	for (m = 0; m < resid;  m++) {
		i = 0;
		if (&orig[m] >= (int8_t *)sc->sc_pr.s.end) { 
			orig = (int8_t *)sc->sc_pr.s.start;
			resid -= m;
			m = 0;
		}
		if (orig[m] != 0) {
			if (orig[m] > 0)
				i = INT8_MAX / orig[m];
			else
				i = INT8_MIN / orig[m];
		}
		if (i > sc->sc_opens)
			i = sc->sc_opens;
		orig[m] *= i;
	}
}

void
saturate_func16(struct audio_softc *sc)
{
	int blksize, m, i, resid;
	int16_t *orig;

	blksize = sc->sc_vchan[0]->sc_mpr.blksize;
	resid = blksize;
	if (sc->sc_trigger_started == false)
		resid *= 2;

	orig = (int16_t *)(sc->sc_pr.s.inp);

	for (m = 0; m < resid / 2;  m++) {
		i = 0;
		if (&orig[m] >= (int16_t *)sc->sc_pr.s.end) { 
			orig = (int16_t *)sc->sc_pr.s.start;
			resid -= m;
			m = 0;
		}
		if (orig[m] != 0) {
			if (orig[m] > 0)
				i = INT16_MAX / orig[m];
			else
				i = INT16_MIN / orig[m];
		}
		if (i > sc->sc_opens)
			i = sc->sc_opens;
		orig[m] *= i;
	}
}

void
saturate_func32(struct audio_softc *sc)
{
	int blksize, m, i, resid;
	int32_t *orig;

	blksize = sc->sc_vchan[0]->sc_mpr.blksize;
	resid = blksize;
	if (sc->sc_trigger_started == false)
		resid *= 2;

	orig = (int32_t *)(sc->sc_pr.s.inp);

	for (m = 0; m < resid / 4;  m++) {
		i = 0;
		if (&orig[m] >= (int32_t *)sc->sc_pr.s.end) { 
			orig = (int32_t *)sc->sc_pr.s.start;
			resid -= m;
			m = 0;
		}
		if (orig[m] != 0) {
			if (orig[m] > 0)
				i = INT32_MAX / orig[m];
			else
				i = INT32_MIN / orig[m];
		}
		if (i > sc->sc_opens)
			i = sc->sc_opens;
		orig[m] *= i;
	}
}

void
saturate_func(struct audio_softc *sc)
{
	switch (sc->sc_precision) {
	case 8:
		saturate_func8(sc);
		break;
	case 16:
		saturate_func16(sc);
		break;
	case 24:
	case 32:
		saturate_func32(sc);
		break;
	default:
		break;
	}
}

void
recswvol_func8(struct audio_softc *sc, struct audio_ringbuffer *cb, int n,
    size_t blksize)
{
	int cc, cc1, m, resid;
	int8_t *orig;

	orig = cb->s.inp;
	resid = blksize;

	while (resid > 0) {
		cc = resid;
		cc1 = cb->s.end - (uint8_t *)orig;
		if (cc > cc1)
			cc = cc1;

		for (m = 0; m < cc; m++) {
			orig[m] = (int16_t)(orig[m] *
			    (int16_t)(sc->sc_vchan[n]->sc_recswvol) / 256);
		}
		orig = cb->s.start;

		resid -= cc;
	}
}

void
recswvol_func16(struct audio_softc *sc, struct audio_ringbuffer *cb, int n,
    size_t blksize)
{
	int cc, cc1, m, resid;
	int16_t *orig;

	orig = (int16_t *)cb->s.inp;
	resid = blksize;

	while (resid > 0) {
		cc = resid;
		cc1 = cb->s.end - (uint8_t *)orig;
		if (cc > cc1)
			cc = cc1;

		for (m = 0; m < cc / 2; m++) {
			orig[m] = (int32_t)(orig[m] *
			    (int32_t)(sc->sc_vchan[n]->sc_recswvol) / 256);
		}
		orig = (uint16_t *)cb->s.start;

		resid -= cc;
	}
}

void
recswvol_func32(struct audio_softc *sc, struct audio_ringbuffer *cb, int n,
    size_t blksize)
{
	int cc, cc1, m, resid;
	int32_t *orig;

	orig = (int32_t *)cb->s.inp;
	resid = blksize;

	while (resid > 0) {
		cc = resid;
		cc1 = cb->s.end - (uint8_t *)orig;
		if (cc > cc1)
			cc = cc1;

		for (m = 0; m < cc / 2; m++) {
			orig[m] = (int64_t)(orig[m] *
			    (int64_t)(sc->sc_vchan[n]->sc_recswvol) / 256);
		}
		orig = (uint32_t *)cb->s.start;

		resid -= cc;
	}
}

void
recswvol_func(struct audio_softc *sc, struct audio_ringbuffer *cb, int n,
    size_t blksize)
{
	switch (sc->sc_precision) {
	case 8:
		recswvol_func8(sc, cb, n, blksize);
		break;
	case 16:
		recswvol_func16(sc, cb, n, blksize);
		break;
	case 24:
	case 32:
		recswvol_func32(sc, cb, n, blksize);
		break;
	default:
		break;
	}
}

static uint8_t *
find_vchan_vol(struct audio_softc *sc, int d)
{
	size_t j, n = (size_t)d / 2;
	
	for (size_t i = j = 0; i <= n; i++)
		if (sc->sc_audiopid[i].pid != -1)
			j++;

	return (d & 1) == 0 ?
	    &sc->sc_vchan[j]->sc_swvol : &sc->sc_vchan[j]->sc_recswvol;
}

static int
audio_set_port(struct audio_softc *sc, mixer_ctrl_t *mc)
{
	KASSERT(mutex_owned(sc->sc_lock));

	int d = mc->dev - sc->sc_static_nmixer_states;

	if (d < 0)
		return sc->hw_if->set_port(sc->hw_hdl, mc);

	uint8_t *level = &mc->un.value.level[AUDIO_MIXER_LEVEL_MONO];
	uint8_t *vol = find_vchan_vol(sc, d);
	*vol = *level;
	return 0;
}


static int
audio_get_port(struct audio_softc *sc, mixer_ctrl_t *mc)
{

	KASSERT(mutex_owned(sc->sc_lock));

	int d = mc->dev - sc->sc_static_nmixer_states;

	if (d < 0)
		return sc->hw_if->get_port(sc->hw_hdl, mc);

	u_char *level = &mc->un.value.level[AUDIO_MIXER_LEVEL_MONO];
	uint8_t *vol = find_vchan_vol(sc, d);
	*level = *vol;
	return 0;

}

static int
audio_query_devinfo(struct audio_softc *sc, mixer_devinfo_t *di)
{
	char mixLabel[255];

	KASSERT(mutex_owned(sc->sc_lock));

	if (di->index >= sc->sc_static_nmixer_states && di->index <
	    sc->sc_nmixer_states) {
		if ((di->index - sc->sc_static_nmixer_states) % 2 == 0) {
			di->mixer_class = AUDIO_OUTPUT_CLASS;
			snprintf(mixLabel, sizeof(mixLabel), AudioNdac"%d",
			    (di->index - sc->sc_static_nmixer_states) / 2);
			strcpy(di->label.name, mixLabel);
			di->type = AUDIO_MIXER_VALUE;
			di->next = di->prev = AUDIO_MIXER_LAST;
			di->un.v.num_channels = 1;
			strcpy(di->un.v.units.name, AudioNvolume);
		} else {
			di->mixer_class = AUDIO_INPUT_CLASS;
			snprintf(mixLabel, sizeof(mixLabel), AudioNmicrophone
			    "%d", (di->index - sc->sc_static_nmixer_states) /
			     2);
			strcpy(di->label.name, mixLabel);
			di->type = AUDIO_MIXER_VALUE;
			di->next = di->prev = AUDIO_MIXER_LAST;
			di->un.v.num_channels = 1;
			strcpy(di->un.v.units.name, AudioNvolume);
		}

		return 0;
	}


	return sc->hw_if->query_devinfo(sc->hw_hdl, di);
}

static int
audio_set_params(struct audio_softc *sc, int setmode, int usemode,
    audio_params_t *play, audio_params_t *rec,
    stream_filter_list_t *pfil, stream_filter_list_t *rfil, int n)
{
	int error = 0;
	KASSERT(mutex_owned(sc->sc_lock));

	if (n == 0 && sc->hw_if->set_params != NULL) {
		sc->sc_ready = true;
		if (sc->sc_precision == 8)
			play->encoding = rec->encoding = AUDIO_ENCODING_SLINEAR;
		error = sc->hw_if->set_params(sc->hw_hdl, setmode, usemode,
 		    play, rec, pfil, rfil);
		if (error != 0)
			sc->sc_ready = false;

		return error;
	}

	if (setmode & AUMODE_PLAY && auconv_set_converter(sc->sc_format,
	    VAUDIO_NFORMATS, AUMODE_PLAY, play, true, pfil) < 0)
		return EINVAL;

	if (pfil->req_size > 0)
		play = &pfil->filters[0].param;

	if (setmode & AUMODE_RECORD && auconv_set_converter(sc->sc_format,
	    VAUDIO_NFORMATS, AUMODE_RECORD, rec, true, rfil) < 0)
		return EINVAL;

	if (rfil->req_size > 0)
		rec = &rfil->filters[0].param;

	return 0;
}

static int
audio_query_encoding(struct audio_softc *sc, struct audio_encoding *ae)
{
	KASSERT(mutex_owned(sc->sc_lock));

	return auconv_query_encoding(sc->sc_encodings, ae);
}

void
audio_play_thread(void *v)
{
	struct audio_softc *sc;
	
	sc = (struct audio_softc *)v;

	mutex_enter(sc->sc_lock);
	for (;;) {
		cv_wait_sig(&sc->sc_condvar, sc->sc_lock);
		if (sc->sc_dying) {
			mutex_exit(sc->sc_lock);
			kthread_exit(0);
		}

		audio_mix(sc);
	}

}

void
audio_rec_thread(void *v)
{
	struct audio_softc *sc;
	
	sc = (struct audio_softc *)v;

	mutex_enter(sc->sc_lock);
	for (;;) {
		cv_wait_sig(&sc->sc_rcondvar, sc->sc_lock);
		if (sc->sc_dying) {
			mutex_exit(sc->sc_lock);
			kthread_exit(0);
		}

		audio_upmix(sc);
	}

}

/* sysctl helper to set common audio frequency */
static int
audio_sysctl_frequency(SYSCTLFN_ARGS)
{
	struct sysctlnode node;
	struct audio_softc *sc;
	int t, error;

	node = *rnode;
	sc = node.sysctl_data;

	t = sc->sc_iffreq;
	node.sysctl_data = &t;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error || newp == NULL)
		return error;

	mutex_enter(sc->sc_lock);

	/* This may not change when a virtual channel is open */
	if (sc->sc_opens) {
		mutex_exit(sc->sc_lock);
		return EBUSY;
	}

	if (t <= 0) {
		mutex_exit(sc->sc_lock);
		return EINVAL;
	}

	sc->sc_iffreq = t;
	error = audio_set_vchan_defaults(sc, AUMODE_PLAY | AUMODE_PLAY_ALL
	    | AUMODE_RECORD, &sc->sc_format[0], 0);
	if (error)
		aprint_error_dev(sc->sc_dev, "Error setting frequency, "
				 "please check hardware capabilities\n");
	mutex_exit(sc->sc_lock);

	return error;
}

/* sysctl helper to set common audio precision */
static int
audio_sysctl_precision(SYSCTLFN_ARGS)
{
	struct sysctlnode node;
	struct audio_softc *sc;
	int t, error;

	node = *rnode;
	sc = node.sysctl_data;

	t = sc->sc_precision;
	node.sysctl_data = &t;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error || newp == NULL)
		return error;

	mutex_enter(sc->sc_lock);

	/* This may not change when a virtual channel is open */
	if (sc->sc_opens) {
		mutex_exit(sc->sc_lock);
		return EBUSY;
	}

	if (t == 0 || (t != 8 && t != 16 && t != 24 && t != 32)) {
		mutex_exit(sc->sc_lock);
		return EINVAL;
	}

	sc->sc_precision = t;

	if (sc->sc_precision != 8) {
 		sc->sc_format[0].encoding =
#if BYTE_ORDER == LITTLE_ENDIAN
			 AUDIO_ENCODING_SLINEAR_LE;
#else
			 AUDIO_ENCODING_SLINEAR_BE;
#endif
	} else
 		sc->sc_format[0].encoding = AUDIO_ENCODING_SLINEAR_LE;

	error = audio_set_vchan_defaults(sc, AUMODE_PLAY | AUMODE_PLAY_ALL
	    | AUMODE_RECORD, &sc->sc_format[0], 0);
	if (error)
		aprint_error_dev(sc->sc_dev, "Error setting precision, "
				 "please check hardware capabilities\n");
	mutex_exit(sc->sc_lock);

	return error;
}

/* sysctl helper to set common audio channels */
static int
audio_sysctl_channels(SYSCTLFN_ARGS)
{
	struct sysctlnode node;
	struct audio_softc *sc;
	int t, error;

	node = *rnode;
	sc = node.sysctl_data;

	t = sc->sc_channels;
	node.sysctl_data = &t;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error || newp == NULL)
		return error;

	mutex_enter(sc->sc_lock);

	/* This may not change when a virtual channel is open */
	if (sc->sc_opens) {
		mutex_exit(sc->sc_lock);
		return EBUSY;
	}

	if (t <= 0 || (t !=1 && t % 2 != 0)) {
		mutex_exit(sc->sc_lock);
		return EINVAL;
	}

	sc->sc_channels = t;
	error = audio_set_vchan_defaults(sc, AUMODE_PLAY | AUMODE_PLAY_ALL
	    | AUMODE_RECORD, &sc->sc_format[0], 0);
	if (error)
		aprint_error_dev(sc->sc_dev, "Error setting channels, "
				 "please check hardware capabilities\n");
	mutex_exit(sc->sc_lock);

	return error;
}

static int
vchan_autoconfig(struct audio_softc *sc)
{
	struct virtual_channel *vc;
	int error, i, j, k;

	vc = sc->sc_vchan[0];
	error = 0;

	mutex_enter(sc->sc_lock);

	for (i = 0; i < __arraycount(auto_config_precision); i++) {
		sc->sc_precision = auto_config_precision[i];
		for (j = 0; j < __arraycount(auto_config_channels); j++) {
			sc->sc_channels = auto_config_channels[j];
			for (k = 0; k < __arraycount(auto_config_freq); k++) {
				sc->sc_iffreq = auto_config_freq[k];
				error = audio_set_vchan_defaults(sc,
				    AUMODE_PLAY | AUMODE_PLAY_ALL |
				    AUMODE_RECORD, &sc->sc_format[0], 0);
				if (vc->sc_npfilters > 0 &&
				    (vc->sc_mpr.s.param.
					sample_rate != sc->sc_iffreq ||
				    vc->sc_mpr.s.param.
				       precision != sc->sc_precision ||
				    vc->sc_mpr.s.param.
					 channels != sc->sc_channels))
					error = EINVAL;

				if (error == 0) {
					aprint_normal_dev(sc->sc_dev,
					    "Virtual format configured - "
					    "Format SLINEAR, precision %d, "
					    "channels %d, frequency %d\n",
					    sc->sc_precision, sc->sc_channels,
					    sc->sc_iffreq);
					mutex_exit(sc->sc_lock);

					return 0;
				}
			}
		}
	}

	aprint_error_dev(sc->sc_dev, "Virtual format auto config failed!\n"
		     "Please check hardware capabilities\n");
	mutex_exit(sc->sc_lock);

	return EINVAL;
}

#endif /* NAUDIO > 0 */
