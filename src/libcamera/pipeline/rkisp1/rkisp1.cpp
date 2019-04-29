/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2019, Google Inc.
 *
 * rkisp1.cpp - Pipeline handler for Rockchip ISP1
 */

#include <iomanip>
#include <memory>
#include <vector>

#include <linux/media-bus-format.h>

#include <libcamera/camera.h>
#include <libcamera/request.h>
#include <libcamera/stream.h>

#include "camera_sensor.h"
#include "device_enumerator.h"
#include "log.h"
#include "media_device.h"
#include "pipeline_handler.h"
#include "utils.h"
#include "v4l2_device.h"
#include "v4l2_subdevice.h"

namespace libcamera {

LOG_DEFINE_CATEGORY(RkISP1)

class PipelineHandlerRkISP1 : public PipelineHandler
{
public:
	PipelineHandlerRkISP1(CameraManager *manager);
	~PipelineHandlerRkISP1();

	CameraConfiguration streamConfiguration(Camera *camera,
		const std::vector<StreamUsage> &usages) override;
	int configureStreams(Camera *camera,
		const CameraConfiguration &config) override;

	int allocateBuffers(Camera *camera,
		const std::set<Stream *> &streams) override;
	int freeBuffers(Camera *camera,
		const std::set<Stream *> &streams) override;

	int start(Camera *camera) override;
	void stop(Camera *camera) override;

	int queueRequest(Camera *camera, Request *request) override;

	bool match(DeviceEnumerator *enumerator) override;

private:
	class RkISP1CameraData : public CameraData
	{
	public:
		RkISP1CameraData(PipelineHandler *pipe)
			: CameraData(pipe), sensor_(nullptr)
		{
		}

		~RkISP1CameraData()
		{
			delete sensor_;
		}

		Stream stream_;
		CameraSensor *sensor_;
	};

	static constexpr unsigned int RKISP1_BUFFER_COUNT = 4;

	RkISP1CameraData *cameraData(const Camera *camera)
	{
		return static_cast<RkISP1CameraData *>(
			PipelineHandler::cameraData(camera));
	}

	int initLinks();
	int createCamera(MediaEntity *sensor);
	void bufferReady(Buffer *buffer);

	std::shared_ptr<MediaDevice> media_;
	V4L2Subdevice *dphy_;
	V4L2Subdevice *isp_;
	V4L2Device *video_;

	Camera *activeCamera_;
};

PipelineHandlerRkISP1::PipelineHandlerRkISP1(CameraManager *manager)
	: PipelineHandler(manager), dphy_(nullptr), isp_(nullptr),
	  video_(nullptr)
{
}

PipelineHandlerRkISP1::~PipelineHandlerRkISP1()
{
	delete video_;
	delete isp_;
	delete dphy_;

	if (media_)
		media_->release();
}

/* -----------------------------------------------------------------------------
 * Pipeline Operations
 */

CameraConfiguration PipelineHandlerRkISP1::streamConfiguration(Camera *camera,
	const std::vector<StreamUsage> &usages)
{
	RkISP1CameraData *data = cameraData(camera);
	CameraConfiguration config;
	StreamConfiguration cfg{};

	cfg.pixelFormat = V4L2_PIX_FMT_NV12;
	cfg.size = data->sensor_->resolution();
	cfg.bufferCount = RKISP1_BUFFER_COUNT;

	config[&data->stream_] = cfg;

	return config;
}

int PipelineHandlerRkISP1::configureStreams(Camera *camera,
					    const CameraConfiguration &config)
{
	RkISP1CameraData *data = cameraData(camera);
	const StreamConfiguration &cfg = config[&data->stream_];
	CameraSensor *sensor = data->sensor_;
	int ret;

	/* Verify the configuration. */
	const Size &resolution = sensor->resolution();
	if (cfg.size.width > resolution.width ||
	    cfg.size.height > resolution.height) {
		LOG(RkISP1, Error)
			<< "Invalid stream size: larger than sensor resolution";
		return -EINVAL;
	}

	/*
	 * Configure the sensor links: enable the link corresponding to this
	 * camera and disable all the other sensor links.
	 */
	const MediaPad *pad = dphy_->entity()->getPadByIndex(0);

	ret = media_->open();
	if (ret < 0)
		return ret;

	for (MediaLink *link : pad->links()) {
		bool enable = link->source()->entity() == sensor->entity();

		if (!!(link->flags() & MEDIA_LNK_FL_ENABLED) == enable)
			continue;

		LOG(RkISP1, Debug)
			<< (enable ? "Enabling" : "Disabling")
			<< " link from sensor '"
			<< link->source()->entity()->name()
			<< "' to CSI-2 receiver";

		ret = link->setEnabled(enable);
		if (ret < 0)
			break;
	}

	media_->close();

	if (ret < 0)
		return ret;

	/*
	 * Configure the format on the sensor output and propagate it through
	 * the pipeline.
	 */
	V4L2SubdeviceFormat format;
	format = sensor->getFormat({ MEDIA_BUS_FMT_SBGGR12_1X12,
				     MEDIA_BUS_FMT_SGBRG12_1X12,
				     MEDIA_BUS_FMT_SGRBG12_1X12,
				     MEDIA_BUS_FMT_SRGGB12_1X12,
				     MEDIA_BUS_FMT_SBGGR10_1X10,
				     MEDIA_BUS_FMT_SGBRG10_1X10,
				     MEDIA_BUS_FMT_SGRBG10_1X10,
				     MEDIA_BUS_FMT_SRGGB10_1X10,
				     MEDIA_BUS_FMT_SBGGR8_1X8,
				     MEDIA_BUS_FMT_SGBRG8_1X8,
				     MEDIA_BUS_FMT_SGRBG8_1X8,
				     MEDIA_BUS_FMT_SRGGB8_1X8 },
				   cfg.size);

	LOG(RkISP1, Debug) << "Configuring sensor with " << format.toString();

	ret = sensor->setFormat(&format);
	if (ret < 0)
		return ret;

	LOG(RkISP1, Debug) << "Sensor configured with " << format.toString();

	ret = dphy_->setFormat(0, &format);
	if (ret < 0)
		return ret;

	ret = dphy_->getFormat(1, &format);
	if (ret < 0)
		return ret;

	ret = isp_->setFormat(0, &format);
	if (ret < 0)
		return ret;

	V4L2DeviceFormat outputFormat = {};
	outputFormat.fourcc = cfg.pixelFormat;
	outputFormat.size = cfg.size;
	outputFormat.planesCount = 2;

	ret = video_->setFormat(&outputFormat);
	if (ret)
		return ret;

	if (outputFormat.size != cfg.size ||
	    outputFormat.fourcc != cfg.pixelFormat) {
		LOG(RkISP1, Error)
			<< "Unable to configure capture in " << cfg.toString();
		return -EINVAL;
	}

	return 0;
}

int PipelineHandlerRkISP1::allocateBuffers(Camera *camera,
					   const std::set<Stream *> &streams)
{
	Stream *stream = *streams.begin();
	return video_->exportBuffers(&stream->bufferPool());
}

int PipelineHandlerRkISP1::freeBuffers(Camera *camera,
				       const std::set<Stream *> &streams)
{
	if (video_->releaseBuffers())
		LOG(RkISP1, Error) << "Failed to release buffers";

	return 0;
}

int PipelineHandlerRkISP1::start(Camera *camera)
{
	int ret;

	ret = video_->streamOn();
	if (ret)
		LOG(RkISP1, Error)
			<< "Failed to start camera " << camera->name();

	activeCamera_ = camera;

	return ret;
}

void PipelineHandlerRkISP1::stop(Camera *camera)
{
	int ret;

	ret = video_->streamOff();
	if (ret)
		LOG(RkISP1, Warning)
			<< "Failed to stop camera " << camera->name();

	PipelineHandler::stop(camera);

	activeCamera_ = nullptr;
}

int PipelineHandlerRkISP1::queueRequest(Camera *camera, Request *request)
{
	RkISP1CameraData *data = cameraData(camera);
	Stream *stream = &data->stream_;

	Buffer *buffer = request->findBuffer(stream);
	if (!buffer) {
		LOG(RkISP1, Error)
			<< "Attempt to queue request with invalid stream";
		return -ENOENT;
	}

	int ret = video_->queueBuffer(buffer);
	if (ret < 0)
		return ret;

	PipelineHandler::queueRequest(camera, request);

	return 0;
}

/* -----------------------------------------------------------------------------
 * Match and Setup
 */

int PipelineHandlerRkISP1::initLinks()
{
	MediaLink *link;
	int ret;

	ret = media_->disableLinks();
	if (ret < 0)
		return ret;

	link = media_->link("rockchip-sy-mipi-dphy", 1, "rkisp1-isp-subdev", 0);
	if (!link)
		return -ENODEV;

	ret = link->setEnabled(true);
	if (ret < 0)
		return ret;

	link = media_->link("rkisp1-isp-subdev", 2, "rkisp1_mainpath", 0);
	if (!link)
		return -ENODEV;

	ret = link->setEnabled(true);
	if (ret < 0)
		return ret;

	return 0;
}

int PipelineHandlerRkISP1::createCamera(MediaEntity *sensor)
{
	int ret;

	std::unique_ptr<RkISP1CameraData> data =
		utils::make_unique<RkISP1CameraData>(this);

	data->sensor_ = new CameraSensor(sensor);
	ret = data->sensor_->init();
	if (ret)
		return ret;

	std::set<Stream *> streams{ &data->stream_ };
	std::shared_ptr<Camera> camera =
		Camera::create(this, sensor->name(), streams);
	registerCamera(std::move(camera), std::move(data));

	return 0;
}

bool PipelineHandlerRkISP1::match(DeviceEnumerator *enumerator)
{
	const MediaPad *pad;
	int ret;

	DeviceMatch dm("rkisp1");
	dm.add("rkisp1-isp-subdev");
	dm.add("rkisp1_selfpath");
	dm.add("rkisp1_mainpath");
	dm.add("rkisp1-statistics");
	dm.add("rkisp1-input-params");
	dm.add("rockchip-sy-mipi-dphy");

	media_ = enumerator->search(dm);
	if (!media_)
		return false;

	media_->acquire();

	ret = media_->open();
	if (ret < 0)
		return ret;

	/* Create the V4L2 subdevices we will need. */
	dphy_ = V4L2Subdevice::fromEntityName(media_.get(),
					      "rockchip-sy-mipi-dphy");
	ret = dphy_->open();
	if (ret < 0)
		goto done;

	isp_ = V4L2Subdevice::fromEntityName(media_.get(), "rkisp1-isp-subdev");
	ret = isp_->open();
	if (ret < 0)
		goto done;

	/* Locate and open the capture video node. */
	video_ = V4L2Device::fromEntityName(media_.get(), "rkisp1_mainpath");
	ret = video_->open();
	if (ret < 0)
		goto done;

	video_->bufferReady.connect(this, &PipelineHandlerRkISP1::bufferReady);

	/* Configure default links. */
	ret = initLinks();
	if (ret < 0) {
		LOG(RkISP1, Error) << "Failed to setup links";
		goto done;
	}

	/*
	 * Enumerate all sensors connected to the CSI-2 receiver and create one
	 * camera instance for each of them.
	 */
	pad = dphy_->entity()->getPadByIndex(0);
	if (!pad) {
		ret = -EINVAL;
		goto done;
	}

	for (MediaLink *link : pad->links())
		createCamera(link->source()->entity());

done:
	media_->close();

	return ret == 0;
}

/* -----------------------------------------------------------------------------
 * Buffer Handling
 */

void PipelineHandlerRkISP1::bufferReady(Buffer *buffer)
{
	ASSERT(activeCamera_);

	RkISP1CameraData *data = cameraData(activeCamera_);
	Request *request = data->queuedRequests_.front();

	completeBuffer(activeCamera_, request, buffer);
	completeRequest(activeCamera_, request);
}

REGISTER_PIPELINE_HANDLER(PipelineHandlerRkISP1);

} /* namespace libcamera */
