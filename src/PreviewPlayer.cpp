/*
Replay Clip Editor for OBS Studio
Copyright (C) 2026 Terra Firma Entertainment <terrafirmaentertainment@gmail.com>
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "PreviewPlayer.hpp"

#include <QMouseEvent>
#include <QResizeEvent>
#include <QVBoxLayout>

#include <algorithm>

/* --------------------------------------------------- preview source type */

static const char *preview_source_get_name(void *)
{
	return "Replay Clip Editor Preview";
}

static void *preview_source_create(obs_data_t *, obs_source_t *)
{
	return bzalloc(1);
}

static void preview_source_destroy(void *data)
{
	bfree(data);
}

void register_preview_source()
{
	static struct obs_source_info info = {};
	info.id = "replay-clip-editor-preview-source";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE |
			    OBS_SOURCE_CAP_DISABLED;
	info.get_name = preview_source_get_name;
	info.create = preview_source_create;
	info.destroy = preview_source_destroy;
	obs_register_source(&info);
}

/* ------------------------------------------------------- ObsDisplayWidget */

ObsDisplayWidget::ObsDisplayWidget(QWidget *parent) : QWidget(parent)
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_NativeWindow);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
}

ObsDisplayWidget::~ObsDisplayWidget()
{
	shutdown();
}

void ObsDisplayWidget::destroyDisplay()
{
	if (display) {
		obs_display_remove_draw_callback(display, drawCallback, this);
		obs_display_destroy(display);
		display = nullptr;
	}
	boundHwnd = nullptr;
}

void ObsDisplayWidget::shutdown()
{
	destroyDisplay();
	source = nullptr;
}

bool ObsDisplayWidget::event(QEvent *event)
{
	/* Qt recreated the platform window (happens when the dialog is closed
	 * and reopened); the display is bound to the dead HWND and would show
	 * as a blank white area. Drop it — show/resize recreates it. */
	if (event->type() == QEvent::WinIdChange && display)
		destroyDisplay();
	return QWidget::event(event);
}

void ObsDisplayWidget::createDisplay()
{
	if (!windowHandle())
		return;
	if (display) {
		if (reinterpret_cast<void *>(winId()) == boundHwnd)
			return;
		destroyDisplay(); /* stale HWND binding */
	}

	gs_init_data info = {};
	QSize size = this->size() * devicePixelRatioF();
	info.cx = std::max(size.width(), 16);
	info.cy = std::max(size.height(), 16);
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;
	info.window.hwnd = reinterpret_cast<void *>(winId());

	/* libobs color byte order is 0xAABBGGRR */
	display = obs_display_create(&info, 0xFF101010);
	if (display) {
		obs_display_add_draw_callback(display, drawCallback, this);
		boundHwnd = info.window.hwnd;
	}
}

void ObsDisplayWidget::drawCallback(void *data, uint32_t cx, uint32_t cy)
{
	auto *self = static_cast<ObsDisplayWidget *>(data);
	obs_source_t *src = self->source;
	if (!src)
		return;

	uint32_t sw = obs_source_get_width(src);
	uint32_t sh = obs_source_get_height(src);
	if (!sw || !sh)
		return;

	float scale = std::min((float)cx / (float)sw, (float)cy / (float)sh);
	int vpW = (int)(sw * scale);
	int vpH = (int)(sh * scale);
	int vpX = ((int)cx - vpW) / 2;
	int vpY = ((int)cy - vpH) / 2;

	gs_viewport_push();
	gs_projection_push();
	gs_ortho(0.0f, (float)sw, 0.0f, (float)sh, -100.0f, 100.0f);
	gs_set_viewport(vpX, vpY, vpW, vpH);

	obs_source_video_render(src);

	gs_projection_pop();
	gs_viewport_pop();
}

void ObsDisplayWidget::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	createDisplay();
}

void ObsDisplayWidget::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	createDisplay();
	if (display) {
		QSize size = event->size() * devicePixelRatioF();
		obs_display_resize(display, size.width(), size.height());
	}
}

void ObsDisplayWidget::paintEvent(QPaintEvent *)
{
	/* obs_display owns this surface. */
}

void ObsDisplayWidget::mousePressEvent(QMouseEvent *event)
{
	if (event->button() == Qt::LeftButton)
		emit clicked();
	QWidget::mousePressEvent(event);
}

/* ---------------------------------------------------------- PreviewPlayer */

PreviewPlayer::PreviewPlayer(QWidget *parent) : QWidget(parent)
{
	setMinimumSize(320, 180);

	displayWidget = new ObsDisplayWidget(this);
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->addWidget(displayWidget);

	/* Clicking the video toggles playback, Steam-style. */
	connect(displayWidget, &ObsDisplayWidget::clicked, this, [this]() {
		if (source)
			togglePlayPause();
	});

	pollTimer.setInterval(50);
	connect(&pollTimer, &QTimer::timeout, this, [this]() {
		if (!source)
			return;
		bool playing = core.isPlaying();
		if (playing != lastPlaying) {
			lastPlaying = playing;
			emit playStateChanged(playing);
		}
		emit positionChanged(core.positionMs());
	});
}

PreviewPlayer::~PreviewPlayer()
{
	shutdown();
}

void PreviewPlayer::shutdown()
{
	pollTimer.stop();
	core.close();
	if (displayWidget)
		displayWidget->shutdown();
	if (source) {
		obs_source_dec_active(source);
		obs_source_dec_showing(source);
		obs_source_release(source);
		source = nullptr;
	}
}

void PreviewPlayer::openFile(const QString &path, double fileFps, qint64 fileDurationMs,
			     const QVector<int> &enabledAudioStreams)
{
	closeFile();

	fps = fileFps > 1.0 ? fileFps : 60.0;
	probedDurationMs = fileDurationMs;

	source = obs_source_create_private("replay-clip-editor-preview-source", "replay-clip-editor-preview",
					   nullptr);
	if (!source)
		return;

	obs_source_set_monitoring_type(source, OBS_MONITORING_TYPE_MONITOR_ONLY);
	obs_source_set_volume(source, monitorVolume);
	/* We pace frames ourselves; OBS's async timing buffer would only add
	 * a visible delay at every play start. */
	obs_source_set_async_unbuffered(source, true);
	obs_source_inc_showing(source);
	obs_source_inc_active(source);
	displayWidget->setSource(source);

	/* Open paused on the first frame; the user starts playback. */
	core.setQuality(previewQuality);
	core.open(path, source, enabledAudioStreams, 0, false);

	lastPlaying = false;
	pollTimer.start();
}

void PreviewPlayer::closeFile()
{
	pollTimer.stop();
	core.close();
	if (displayWidget)
		displayWidget->setSource(nullptr);
	if (source) {
		obs_source_dec_active(source);
		obs_source_dec_showing(source);
		obs_source_release(source);
		source = nullptr;
	}
}

void PreviewPlayer::play()
{
	if (!source)
		return;
	if (core.isEnded())
		core.seekMs(0);
	core.play();
}

void PreviewPlayer::pause()
{
	core.pause();
	if (lastPlaying) {
		lastPlaying = false;
		emit playStateChanged(false);
	}
}

void PreviewPlayer::togglePlayPause()
{
	if (core.isPlaying())
		pause();
	else
		play();
}

void PreviewPlayer::scrubTo(qint64 ms)
{
	if (!source)
		return;
	core.pause();
	core.seekMs(std::clamp<qint64>(ms, 0, probedDurationMs));
	if (lastPlaying) {
		lastPlaying = false;
		emit playStateChanged(false);
	}
	emit positionChanged(core.positionMs());
}

void PreviewPlayer::seekTo(qint64 ms)
{
	if (!source)
		return;
	core.seekMs(std::clamp<qint64>(ms, 0, probedDurationMs));
	emit positionChanged(core.positionMs());
}

void PreviewPlayer::stepFrames(int count)
{
	if (!source)
		return;
	qint64 frameMs = std::max<qint64>((qint64)(1000.0 / fps), 1);
	scrubTo(core.positionMs() + count * frameMs);
}

void PreviewPlayer::setEnabledTracks(const QVector<int> &streams)
{
	core.setEnabledTracks(streams);
}

void PreviewPlayer::setPreviewQuality(int level)
{
	previewQuality = level;
	core.setQuality(level);
}

void PreviewPlayer::setMonitorVolume(float volume)
{
	monitorVolume = volume;
	if (source)
		obs_source_set_volume(source, volume);
}
