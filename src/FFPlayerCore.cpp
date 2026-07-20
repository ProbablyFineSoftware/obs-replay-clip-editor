/*
Replay Clip Editor for OBS Studio
Copyright (C) 2026 Terra Firma Entertainment <terrafirmaentertainment@gmail.com>
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "FFPlayerCore.hpp"

#include <plugin-support.h>
#include <util/platform.h>
#include <util/threading.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>
#include <deque>
#include <vector>

namespace {

constexpr int kMaxSeekDecodeFrames = 900; /* safety cap while rolling forward */
/* Frames decoded ahead of display. Audio interleaved with them is delivered
 * to the monitor this far in advance, giving it scheduling margin. */
constexpr int kVideoQueueLen = 5;

struct AudioStream {
	int streamIndex = -1;
	AVCodecContext *dec = nullptr;
	AVFilterContext *src = nullptr; /* owned by graph */
};

enum video_format mapPixelFormat(AVPixelFormat fmt)
{
	switch (fmt) {
	case AV_PIX_FMT_YUV420P:
	case AV_PIX_FMT_YUVJ420P:
		return VIDEO_FORMAT_I420;
	case AV_PIX_FMT_NV12:
		return VIDEO_FORMAT_NV12;
	case AV_PIX_FMT_YUV422P:
	case AV_PIX_FMT_YUVJ422P:
		return VIDEO_FORMAT_I422;
	case AV_PIX_FMT_YUV444P:
	case AV_PIX_FMT_YUVJ444P:
		return VIDEO_FORMAT_I444;
	default:
		return VIDEO_FORMAT_NONE;
	}
}

enum video_colorspace mapColorspace(const AVFrame *f)
{
	switch (f->colorspace) {
	case AVCOL_SPC_BT709:
		return VIDEO_CS_709;
	case AVCOL_SPC_SMPTE170M:
	case AVCOL_SPC_BT470BG:
		return VIDEO_CS_601;
	default:
		return f->height >= 720 ? VIDEO_CS_709 : VIDEO_CS_601;
	}
}

struct Pipeline {
	AVFormatContext *fmt = nullptr;  /* video side */
	AVFormatContext *afmt = nullptr; /* independent audio side — re-syncs
					    to the playhead on every resume */
	int videoIdx = -1;
	AVCodecContext *vdec = nullptr;
	std::vector<AudioStream> audio; /* every audio stream in the file */

	AVFilterGraph *graph = nullptr;
	AVFilterContext *sink = nullptr;

	SwsContext *sws = nullptr;
	AVFrame *swsFrame = nullptr;

	AVPacket *pkt = nullptr;
	AVPacket *apkt = nullptr;
	AVFrame *frame = nullptr;
	AVFrame *filtFrame = nullptr;

	qint64 durationMs = 0;

	~Pipeline()
	{
		if (sws)
			sws_freeContext(sws);
		av_frame_free(&swsFrame);
		av_frame_free(&frame);
		av_frame_free(&filtFrame);
		av_packet_free(&pkt);
		av_packet_free(&apkt);
		avcodec_free_context(&vdec);
		for (auto &a : audio)
			avcodec_free_context(&a.dec);
		avfilter_graph_free(&graph);
		avformat_close_input(&fmt);
		avformat_close_input(&afmt);
	}

	bool open(const QString &path)
	{
		if (avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr) < 0)
			return false;
		if (avformat_find_stream_info(fmt, nullptr) < 0)
			return false;
		if (fmt->duration != AV_NOPTS_VALUE)
			durationMs = fmt->duration / (AV_TIME_BASE / 1000);

		videoIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
		if (videoIdx < 0)
			return false;
		AVStream *vst = fmt->streams[videoIdx];
		const AVCodec *vc = avcodec_find_decoder(vst->codecpar->codec_id);
		vdec = avcodec_alloc_context3(vc);
		avcodec_parameters_to_context(vdec, vst->codecpar);
		vdec->thread_count = 0;
		if (avcodec_open2(vdec, vc, nullptr) < 0)
			return false;

		bool hasAudioStreams = false;
		for (unsigned i = 0; i < fmt->nb_streams; i++) {
			AVStream *st = fmt->streams[i];
			if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
				hasAudioStreams = true;
				/* the video side never touches audio packets */
				st->discard = AVDISCARD_ALL;
			}
		}

		if (hasAudioStreams && avformat_open_input(&afmt, path.toUtf8().constData(), nullptr, nullptr) >= 0 &&
		    avformat_find_stream_info(afmt, nullptr) >= 0) {
			for (unsigned i = 0; i < afmt->nb_streams; i++) {
				AVStream *st = afmt->streams[i];
				if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
					st->discard = AVDISCARD_ALL;
					continue;
				}
				const AVCodec *ac = avcodec_find_decoder(st->codecpar->codec_id);
				AudioStream as;
				as.streamIndex = (int)i;
				as.dec = avcodec_alloc_context3(ac);
				avcodec_parameters_to_context(as.dec, st->codecpar);
				if (avcodec_open2(as.dec, ac, nullptr) < 0) {
					avcodec_free_context(&as.dec);
					continue;
				}
				audio.push_back(as);
			}
		}

		pkt = av_packet_alloc();
		apkt = av_packet_alloc();
		frame = av_frame_alloc();
		filtFrame = av_frame_alloc();
		return true;
	}

	/* Build (or rebuild) the mixing graph over the enabled streams. */
	bool buildGraph(const QVector<int> &enabled)
	{
		avfilter_graph_free(&graph);
		sink = nullptr;
		for (auto &a : audio)
			a.src = nullptr;

		std::vector<AudioStream *> inputs;
		for (auto &a : audio) {
			bool on = enabled.contains(a.streamIndex);
			if (afmt)
				afmt->streams[a.streamIndex]->discard = on ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
			if (on)
				inputs.push_back(&a);
		}
		if (inputs.empty())
			return true; /* no audio — valid state */

		graph = avfilter_graph_alloc();

		QStringList inNames;
		for (size_t i = 0; i < inputs.size(); i++) {
			AudioStream *a = inputs[i];
			AVStream *st = afmt->streams[a->streamIndex];
			char layout[128];
			av_channel_layout_describe(&a->dec->ch_layout, layout, sizeof(layout));
			QByteArray args =
				QStringLiteral("time_base=%1/%2:sample_rate=%3:sample_fmt=%4:channel_layout=%5")
					.arg(st->time_base.num)
					.arg(st->time_base.den)
					.arg(a->dec->sample_rate)
					.arg(QString::fromUtf8(av_get_sample_fmt_name(a->dec->sample_fmt)))
					.arg(QString::fromUtf8(layout))
					.toUtf8();
			QByteArray name = QStringLiteral("in%1").arg(i).toUtf8();
			if (avfilter_graph_create_filter(&a->src, avfilter_get_by_name("abuffer"), name.constData(),
							 args.constData(), nullptr, graph) < 0)
				return false;
			inNames << QString::fromUtf8(name);
		}

		if (avfilter_graph_create_filter(&sink, avfilter_get_by_name("abuffersink"), "out", nullptr, nullptr,
						 graph) < 0)
			return false;
		static const enum AVSampleFormat sinkFmts[] = {AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_NONE};
		av_opt_set_int_list(sink, "sample_fmts", sinkFmts, AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN);

		QString desc;
		if (inputs.size() > 1) {
			for (const QString &n : inNames)
				desc += QStringLiteral("[%1]").arg(n);
			desc += QStringLiteral("amix=inputs=%1:normalize=0,").arg(inputs.size());
		} else {
			desc = QStringLiteral("[%1]").arg(inNames[0]);
		}
		desc += QStringLiteral("aresample=48000,aformat=sample_fmts=fltp:channel_layouts=stereo[out]");

		AVFilterInOut *gIn = nullptr;
		AVFilterInOut *gOut = nullptr;
		int ret = avfilter_graph_parse2(graph, desc.toUtf8().constData(), &gIn, &gOut);
		if (ret >= 0) {
			int i = 0;
			for (AVFilterInOut *cur = gIn; cur && ret >= 0; cur = cur->next, i++) {
				int srcIdx = i;
				if (cur->name)
					sscanf(cur->name, "in%d", &srcIdx);
				if (srcIdx >= 0 && srcIdx < (int)inputs.size())
					ret = avfilter_link(inputs[srcIdx]->src, 0, cur->filter_ctx, cur->pad_idx);
			}
			if (ret >= 0 && gOut)
				ret = avfilter_link(gOut->filter_ctx, gOut->pad_idx, sink, 0);
		}
		avfilter_inout_free(&gIn);
		avfilter_inout_free(&gOut);
		if (ret < 0)
			return false;
		return avfilter_graph_config(graph, nullptr) >= 0;
	}

	void flushDecoders()
	{
		avcodec_flush_buffers(vdec);
		for (auto &a : audio)
			avcodec_flush_buffers(a.dec);
	}

	qint64 frameMs(const AVFrame *f, int streamIndex) const
	{
		int64_t pts = f->best_effort_timestamp;
		if (pts == AV_NOPTS_VALUE)
			return -1;
		return av_rescale_q(pts, fmt->streams[streamIndex]->time_base, AVRational{1, 1000});
	}
};

} // namespace

FFPlayerCore::~FFPlayerCore()
{
	close();
}

void FFPlayerCore::open(const QString &filePath, obs_source_t *outputSource, const QVector<int> &enabledAudioStreams,
			qint64 startMs, bool startPlaying)
{
	close();
	path = filePath;
	source = outputSource;
	{
		std::lock_guard<std::mutex> lk(cmdMutex);
		quitFlag = false;
		wantPlaying = startPlaying;
		pendingSeekMs = std::max<int64_t>(startMs, 0);
		tracksDirty = false;
		enabledStreams = enabledAudioStreams;
	}
	posMs = startMs;
	state = 0;
	worker = std::thread([this]() { threadLoop(); });
}

void FFPlayerCore::close()
{
	{
		std::lock_guard<std::mutex> lk(cmdMutex);
		quitFlag = true;
	}
	cmdCv.notify_all();
	if (worker.joinable())
		worker.join();
	if (source) {
		obs_source_output_video(source, nullptr); /* clear last frame */
		source = nullptr;
	}
}

void FFPlayerCore::play()
{
	{
		std::lock_guard<std::mutex> lk(cmdMutex);
		wantPlaying = true;
	}
	cmdCv.notify_all();
}

void FFPlayerCore::pause()
{
	{
		std::lock_guard<std::mutex> lk(cmdMutex);
		wantPlaying = false;
	}
	/* Reflect the pause immediately so play/pause toggles can't misread
	 * a stale "playing" state while the worker winds down. */
	int expected = 1;
	state.compare_exchange_strong(expected, 0);
	/* Silence the audio delivered ahead right now — the worker may be
	 * busy (e.g. mid-seek) and mute too late otherwise. */
	if (source)
		obs_source_set_muted(source, true);
	cmdCv.notify_all();
}

void FFPlayerCore::seekMs(qint64 ms)
{
	{
		std::lock_guard<std::mutex> lk(cmdMutex);
		pendingSeekMs = std::max<qint64>(ms, 0);
	}
	posMs = std::max<qint64>(ms, 0); /* optimistic; corrected after decode */
	/* Stop the old position's look-ahead audio immediately; playback
	 * unmutes once it resumes at the new position. */
	if (source)
		obs_source_set_muted(source, true);
	cmdCv.notify_all();
}

void FFPlayerCore::setEnabledTracks(const QVector<int> &streams)
{
	{
		std::lock_guard<std::mutex> lk(cmdMutex);
		enabledStreams = streams;
		tracksDirty = true;
	}
	cmdCv.notify_all();
}

void FFPlayerCore::threadLoop()
{
	os_set_thread_name("replay-clip-editor-preview");

	Pipeline p;
	if (!p.open(path)) {
		obs_log(LOG_WARNING, "preview: failed to open '%s'", path.toUtf8().constData());
		state = 2;
		return;
	}

	QVector<int> tracks;
	{
		std::lock_guard<std::mutex> lk(cmdMutex);
		tracks = enabledStreams;
	}
	p.buildGraph(tracks);

	int64_t playStartNs = 0;
	int64_t startMediaMs = 0;
	int64_t decoderPosMs = -1;  /* last frame actually shown; posMs may be
				       set optimistically by the UI */
	int64_t lastDecodedMs = -1; /* how far the demuxer/decoder has read
				       (the look-ahead queue runs ahead of
				       what's displayed) */
	bool ended = false;
	bool readEof = false;
	std::deque<AVFrame *> videoQueue;

	auto clearVideoQueue = [&]() {
		for (AVFrame *f : videoQueue)
			av_frame_free(&f);
		videoQueue.clear();
	};

	/* Wait until deadline (ns, os_gettime clock); false if interrupted. */
	auto sleepUntilNs = [&](int64_t deadlineNs) {
		int64_t delta = deadlineNs - (int64_t)os_gettime_ns();
		if (delta <= 0)
			return true;
		std::unique_lock<std::mutex> lk(cmdMutex);
		return !cmdCv.wait_for(lk, std::chrono::nanoseconds(delta), [&] {
			return quitFlag || pendingSeekMs >= 0 || tracksDirty || !wantPlaying;
		});
	};

	auto outputVideoFrame = [&](AVFrame *f) {
		AVFrame *out = f;
		enum video_format vfmt = mapPixelFormat((AVPixelFormat)f->format);
		if (vfmt == VIDEO_FORMAT_NONE) {
			if (!p.sws) {
				p.sws = sws_getContext(f->width, f->height, (AVPixelFormat)f->format, f->width,
						       f->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr,
						       nullptr);
				p.swsFrame = av_frame_alloc();
				p.swsFrame->width = f->width;
				p.swsFrame->height = f->height;
				p.swsFrame->format = AV_PIX_FMT_YUV420P;
				av_frame_get_buffer(p.swsFrame, 0);
			}
			if (!p.sws)
				return;
			sws_scale(p.sws, f->data, f->linesize, 0, f->height, p.swsFrame->data, p.swsFrame->linesize);
			out = p.swsFrame;
			out->colorspace = f->colorspace;
			out->color_range = f->color_range;
			vfmt = VIDEO_FORMAT_I420;
		}

		struct obs_source_frame frame = {};
		frame.width = out->width;
		frame.height = out->height;
		frame.format = vfmt;
		frame.timestamp = os_gettime_ns();
		frame.full_range = out->color_range == AVCOL_RANGE_JPEG ||
				   (AVPixelFormat)f->format == AV_PIX_FMT_YUVJ420P;
		for (int i = 0; i < 4; i++) {
			frame.data[i] = out->data[i];
			frame.linesize[i] = (uint32_t)std::abs(out->linesize[i]);
		}
		video_format_get_parameters_for_format(mapColorspace(out),
						       frame.full_range ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL, vfmt,
						       frame.color_matrix, frame.color_range_min,
						       frame.color_range_max);
		obs_source_output_video(source, &frame);
	};

	auto drainAudioSink = [&]() {
		if (!p.sink)
			return;
		while (av_buffersink_get_frame(p.sink, p.filtFrame) >= 0) {
			qint64 ams = av_rescale_q(p.filtFrame->pts, av_buffersink_get_time_base(p.sink),
						  AVRational{1, 1000});
			int64_t due = playStartNs + (ams - startMediaMs) * 1000000LL;
			int64_t nowNs = (int64_t)os_gettime_ns();
			if (due < nowNs)
				due = nowNs;

			struct obs_source_audio audio = {};
			audio.data[0] = p.filtFrame->data[0];
			audio.data[1] = p.filtFrame->data[1];
			audio.frames = (uint32_t)p.filtFrame->nb_samples;
			audio.speakers = SPEAKERS_STEREO;
			audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
			audio.samples_per_sec = 48000;
			audio.timestamp = (uint64_t)due;
			obs_source_output_audio(source, &audio);
			av_frame_unref(p.filtFrame);
		}
	};

	int64_t lastAudioMs = 0;
	int64_t audioDropUntilMs = 0;
	bool audioEof = false;

	/* Re-position the independent audio pipeline at ms. Cheap: audio has
	 * no keyframe roll-in. */
	auto audioSeek = [&](int64_t ms) {
		if (!p.afmt || p.audio.empty())
			return;
		av_seek_frame(p.afmt, -1, ms * (AV_TIME_BASE / 1000), AVSEEK_FLAG_BACKWARD);
		for (auto &a : p.audio)
			avcodec_flush_buffers(a.dec);
		p.buildGraph(tracks);
		audioDropUntilMs = ms;
		lastAudioMs = ms;
		audioEof = false;
	};

	/* Keep the monitor fed up to ~150 ms ahead of the video position. */
	auto pumpAudio = [&]() {
		if (!p.afmt || !p.sink || audioEof)
			return;
		while (lastAudioMs - posMs.load() < 150) {
			if (av_read_frame(p.afmt, p.apkt) < 0) {
				audioEof = true;
				break;
			}
			for (auto &a : p.audio) {
				if (a.streamIndex != p.apkt->stream_index || !a.src)
					continue;
				AVStream *ast = p.afmt->streams[a.streamIndex];
				if (avcodec_send_packet(a.dec, p.apkt) >= 0) {
					while (avcodec_receive_frame(a.dec, p.frame) >= 0) {
						int64_t pts = p.frame->best_effort_timestamp;
						int64_t ams = pts != AV_NOPTS_VALUE
								      ? av_rescale_q(pts, ast->time_base,
										     AVRational{1, 1000})
								      : lastAudioMs;
						int64_t durMs = p.frame->nb_samples * 1000LL /
								std::max(p.frame->sample_rate, 1);
						lastAudioMs = std::max(lastAudioMs, ams);
						if (ams + durMs < audioDropUntilMs) {
							av_frame_unref(p.frame);
							continue;
						}
						av_buffersrc_add_frame_flags(a.src, p.frame,
									     AV_BUFFERSRC_FLAG_KEEP_REF);
						av_frame_unref(p.frame);
					}
				}
				break;
			}
			av_packet_unref(p.apkt);
			drainAudioSink();
		}
	};

	/* Seek to targetMs and display the exact frame there. */
	auto doSeek = [&](int64_t targetMs) {
		targetMs = std::clamp<int64_t>(targetMs, 0, std::max<qint64>(p.durationMs - 1, 0));

		/* Short forward hops (frame steps, small scrubs) continue from
		 * the current decoder position instead of re-decoding the whole
		 * GOP from the previous keyframe. */
		if (decoderPosMs >= 0 && !ended && targetMs == decoderPosMs) {
			/* Already showing this exact frame. */
			posMs = decoderPosMs;
			playStartNs = (int64_t)os_gettime_ns();
			startMediaMs = decoderPosMs;
			return;
		}

		clearVideoQueue();
		readEof = false;

		/* Roll forward only when the target is ahead of where the
		 * decoder has actually read to (the queue may be ahead of
		 * what's displayed). */
		int64_t demuxMs = lastDecodedMs >= 0 ? lastDecodedMs : decoderPosMs;
		bool rollForward = demuxMs >= 0 && !ended && targetMs > demuxMs && targetMs - demuxMs < 1500;
		if (!rollForward) {
			av_seek_frame(p.fmt, -1, targetMs * (AV_TIME_BASE / 1000), AVSEEK_FLAG_BACKWARD);
			avcodec_flush_buffers(p.vdec);
		}

		/* While far from the target, skip decoding non-reference frames
		 * — they can never be shown, only rolled past. */
		p.vdec->skip_frame = AVDISCARD_NONREF;
		p.vdec->skip_loop_filter = quality.load() >= 1 ? AVDISCARD_ALL : AVDISCARD_DEFAULT;

		qint64 shownMs = targetMs;
		bool shown = false;
		bool aborted = false;
		int decoded = 0;
		AVFrame *hold = av_frame_alloc();
		bool haveHold = false;

		while (!shown && decoded < kMaxSeekDecodeFrames) {
			{
				/* a newer scrub target supersedes this one */
				std::lock_guard<std::mutex> lk(cmdMutex);
				if (pendingSeekMs >= 0 || quitFlag) {
					aborted = true;
					break;
				}
			}
			if (av_read_frame(p.fmt, p.pkt) < 0)
				break;
			if (p.pkt->stream_index != p.videoIdx) {
				av_packet_unref(p.pkt);
				continue;
			}
			if (avcodec_send_packet(p.vdec, p.pkt) >= 0) {
				while (avcodec_receive_frame(p.vdec, p.frame) >= 0) {
					decoded++;
					qint64 ms = p.frameMs(p.frame, p.videoIdx);
					if (ms < 0)
						ms = targetMs;
					/* Close to the target: decode everything again
					 * so the exact frame is available. */
					if (targetMs - ms < 250)
						p.vdec->skip_frame = AVDISCARD_DEFAULT;
					lastDecodedMs = ms;
					av_frame_unref(hold);
					av_frame_ref(hold, p.frame);
					haveHold = true;
					av_frame_unref(p.frame);
					if (ms >= targetMs) {
						shownMs = ms;
						shown = true;
						break;
					}
				}
			}
			av_packet_unref(p.pkt);
		}
		p.vdec->skip_frame = AVDISCARD_DEFAULT;
		if (aborted) {
			/* A newer seek owns the position now — show nothing;
			 * the next seek continues rolling from lastDecodedMs. */
			av_frame_free(&hold);
			return;
		}
		if (haveHold) {
			outputVideoFrame(hold);
			if (!shown)
				shownMs = p.frameMs(hold, p.videoIdx) >= 0 ? p.frameMs(hold, p.videoIdx) : targetMs;
		}
		av_frame_free(&hold);

		posMs = shownMs;
		decoderPosMs = shownMs;
		lastDecodedMs = shownMs;
		playStartNs = (int64_t)os_gettime_ns();
		startMediaMs = shownMs;
		ended = false;
	};

	for (;;) {
		bool doQuit, playing, rebuild;
		int64_t seekTarget;
		{
			std::unique_lock<std::mutex> lk(cmdMutex);
			cmdCv.wait(lk, [&] {
				return quitFlag || pendingSeekMs >= 0 || tracksDirty || (wantPlaying && !ended);
			});
			doQuit = quitFlag;
			seekTarget = pendingSeekMs;
			pendingSeekMs = -1;
			rebuild = tracksDirty;
			tracksDirty = false;
			playing = wantPlaying;
			if (rebuild)
				tracks = enabledStreams;
		}
		if (doQuit)
			break;

		if (rebuild && seekTarget < 0)
			p.buildGraph(tracks);
		if (seekTarget >= 0)
			doSeek(seekTarget);

		if (!playing || ended) {
			state = ended ? 2 : 0;
			continue;
		}

		/* resume timing from the current position; audio re-syncs to
		 * the playhead so playback starts with sound from frame one */
		playStartNs = (int64_t)os_gettime_ns();
		startMediaMs = posMs;
		state = 1;
		audioSeek(posMs);
		obs_source_set_muted(source, false);

		bool interrupted = false;
		while (!interrupted) {
			bool stop = false;
			bool pausing = false;
			{
				std::lock_guard<std::mutex> lk(cmdMutex);
				if (quitFlag || pendingSeekMs >= 0 || tracksDirty || !wantPlaying) {
					stop = true;
					if (!wantPlaying && !ended) {
						state = 0;
						pausing = true;
					}
				}
			}
			if (stop) {
				/* Mute so the audio delivered ahead doesn't ring
				 * out; resume re-syncs audio to the playhead. */
				if (pausing)
					obs_source_set_muted(source, true);
				/* keep the queue: resume continues from it */
				break;
			}

			/* Fill the look-ahead: decode video into the queue and
			 * deliver interleaved audio to the monitor early. */
			while ((int)videoQueue.size() < kVideoQueueLen && !readEof) {
				if (av_read_frame(p.fmt, p.pkt) < 0) {
					avcodec_send_packet(p.vdec, nullptr);
					while (avcodec_receive_frame(p.vdec, p.frame) >= 0) {
						qint64 ms = p.frameMs(p.frame, p.videoIdx);
						if (ms >= 0) {
							videoQueue.push_back(av_frame_clone(p.frame));
							lastDecodedMs = ms;
						}
						av_frame_unref(p.frame);
					}
					readEof = true;
					break;
				}

				if (p.pkt->stream_index == p.videoIdx) {
					int q = quality.load();
					p.vdec->skip_loop_filter = q >= 1 ? AVDISCARD_ALL : AVDISCARD_DEFAULT;
					p.vdec->skip_frame = q >= 2 ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
					if (avcodec_send_packet(p.vdec, p.pkt) >= 0) {
						while (avcodec_receive_frame(p.vdec, p.frame) >= 0) {
							qint64 ms = p.frameMs(p.frame, p.videoIdx);
							if (ms >= 0) {
								videoQueue.push_back(av_frame_clone(p.frame));
								lastDecodedMs = ms;
							}
							av_frame_unref(p.frame);
						}
					}
				}
				av_packet_unref(p.pkt);
			}

			/* audio runs on its own demuxer, always ahead */
			pumpAudio();

			if (videoQueue.empty()) {
				/* end of file, everything shown */
				ended = true;
				state = 2;
				{
					std::lock_guard<std::mutex> lk(cmdMutex);
					wantPlaying = false;
				}
				break;
			}

			AVFrame *f = videoQueue.front();
			qint64 ms = p.frameMs(f, p.videoIdx);
			if (ms < 0)
				ms = posMs;
			if (!sleepUntilNs(playStartNs + (ms - startMediaMs) * 1000000LL)) {
				/* A command (seek/pause) arrived — don't show
				 * this stale frame or clobber the position; a
				 * pending seek owns posMs now. */
				break;
			}
			outputVideoFrame(f);
			posMs = ms;
			decoderPosMs = ms;
			videoQueue.pop_front();
			av_frame_free(&f);
		}
	}

	clearVideoQueue();
}
