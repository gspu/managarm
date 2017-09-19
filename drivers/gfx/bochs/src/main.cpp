
#include <assert.h>
#include <stdio.h>
#include <deque>
#include <experimental/optional>
#include <functional>
#include <iostream>
#include <memory>
#include <numeric>

#include <arch/bits.hpp>
#include <arch/register.hpp>
#include <arch/io_space.hpp>
#include <async/result.hpp>
#include <boost/intrusive/list.hpp>
#include <cofiber.hpp>
#include <frigg/atomic.hpp>
#include <frigg/arch_x86/machine.hpp>
#include <frigg/memory.hpp>
#include <helix/ipc.hpp>
#include <helix/await.hpp>
#include <protocols/fs/server.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/usb/usb.hpp>
#include <protocols/usb/api.hpp>
#include <protocols/usb/server.hpp>

#include <libdrm/drm.h>
#include <libdrm/drm_mode.h>

#include "bochs.hpp"
#include <fs.pb.h>

// ----------------------------------------------------------------
// Stuff that belongs in a DRM library.
// ----------------------------------------------------------------

// ----------------------------------------------------------------
// Device
// ----------------------------------------------------------------

void drm_core::Device::setupCrtc(std::shared_ptr<drm_core::Crtc> crtc) {
	crtc->index = _crtcs.size();
	_crtcs.push_back(crtc);
}

void drm_core::Device::setupEncoder(std::shared_ptr<drm_core::Encoder> encoder) {
	encoder->index = _encoders.size();
	_encoders.push_back(encoder);
}

void drm_core::Device::attachConnector(std::shared_ptr<drm_core::Connector> connector) {
	_connectors.push_back(connector);
}

const std::vector<std::shared_ptr<drm_core::Crtc>> &drm_core::Device::getCrtcs() {
	return _crtcs;
}

const std::vector<std::shared_ptr<drm_core::Encoder>> &drm_core::Device::getEncoders() {
	return _encoders;
}

const std::vector<std::shared_ptr<drm_core::Connector>> &drm_core::Device::getConnectors() {
	return _connectors;
}

void drm_core::Device::registerObject(std::shared_ptr<drm_core::ModeObject> object) {
	_objects.insert({object->id(), object});
}

drm_core::ModeObject *drm_core::Device::findObject(uint32_t id) {
	auto it = _objects.find(id);
	if(it == _objects.end())
		return nullptr;
	return it->second.get();
}

uint64_t drm_core::Device::installMapping(drm_core::BufferObject *bo) {
	auto address = _mappingAllocator.allocate(bo->getSize());
	_mappings.insert({address, bo});
	return address;
}
	
std::pair<uint64_t, drm_core::BufferObject *> drm_core::Device::findMapping(uint64_t offset) {
	auto it = _mappings.upper_bound(offset);
	if(it == _mappings.begin())
		throw std::runtime_error("Mapping does not exist!");
	
	it--;
	return *it;
}

void drm_core::Device::setupMinDimensions(uint32_t width, uint32_t height) {
	_minWidth = width;
	_minHeight = height;
}

void drm_core::Device::setupMaxDimensions(uint32_t width, uint32_t height) {
	_maxWidth = width;
	_maxHeight = height;
}

uint32_t drm_core::Device::getMinWidth() {
	return _minWidth;
}

uint32_t drm_core::Device::getMaxWidth() {
	return _maxWidth;
}

uint32_t drm_core::Device::getMinHeight() {
	return _minHeight;
}

uint32_t drm_core::Device::getMaxHeight() {
	return _maxHeight;
}
	
// ----------------------------------------------------------------
// BufferObject
// ----------------------------------------------------------------

void drm_core::BufferObject::setupMapping(uint64_t mapping) {
	_mapping = mapping;
}

uint64_t drm_core::BufferObject::getMapping() {
	return _mapping;
}

// ----------------------------------------------------------------
// Encoder
// ----------------------------------------------------------------

drm_core::Encoder::Encoder(uint32_t id)
	:drm_core::ModeObject { ObjectType::encoder, id } {
	index = -1;
	_currentCrtc = nullptr;
}

drm_core::Crtc *drm_core::Encoder::currentCrtc() {
	return _currentCrtc;
}

void drm_core::Encoder::setCurrentCrtc(drm_core::Crtc *crtc) {
	_currentCrtc = crtc;
}
	
void drm_core::Encoder::setupEncoderType(uint32_t type) {
	_encoderType = type;
}
	
uint32_t drm_core::Encoder::getEncoderType() {
	return _encoderType;
}
	
void drm_core::Encoder::setupPossibleCrtcs(std::vector<drm_core::Crtc *> crtcs) {
	_possibleCrtcs = crtcs;
}
	
const std::vector<drm_core::Crtc *> &drm_core::Encoder::getPossibleCrtcs() {
	return _possibleCrtcs;
}
	
void drm_core::Encoder::setupPossibleClones(std::vector<drm_core::Encoder *> clones) {
	_possibleClones = clones;
}
	
const std::vector<drm_core::Encoder *> &drm_core::Encoder::getPossibleClones() {
	return _possibleClones;
}

// ----------------------------------------------------------------
// ModeObject
// ----------------------------------------------------------------

uint32_t drm_core::ModeObject::id() {
	return _id;
}

drm_core::Encoder *drm_core::ModeObject::asEncoder() {
	if(_type != ObjectType::encoder)
		return nullptr;
	return static_cast<Encoder *>(this);
}

drm_core::Connector *drm_core::ModeObject::asConnector() {
	if(_type != ObjectType::connector)
		return nullptr;
	return static_cast<Connector *>(this);
}

drm_core::Crtc *drm_core::ModeObject::asCrtc() {
	if(_type != ObjectType::crtc)
		return nullptr;
	return static_cast<Crtc *>(this);
}

drm_core::FrameBuffer *drm_core::ModeObject::asFrameBuffer() {
	if(_type != ObjectType::frameBuffer)
		return nullptr;
	return static_cast<FrameBuffer *>(this);
}

drm_core::Plane *drm_core::ModeObject::asPlane() {
	if(_type != ObjectType::plane)
		return nullptr;
	return static_cast<Plane *>(this);
}

// ----------------------------------------------------------------
// Crtc
// ----------------------------------------------------------------

drm_core::Crtc::Crtc(uint32_t id)
	:drm_core::ModeObject { ObjectType::crtc, id } {
	index = -1;
}

std::shared_ptr<drm_core::Blob> drm_core::Crtc::currentMode() {
	return _curMode;
}
	
void drm_core::Crtc::setCurrentMode(std::shared_ptr<drm_core::Blob> mode) {
	_curMode = mode;
}

// ----------------------------------------------------------------
// FrameBuffer
// ----------------------------------------------------------------

drm_core::FrameBuffer::FrameBuffer(uint32_t id)
	:drm_core::ModeObject { ObjectType::frameBuffer, id } {
}

// ----------------------------------------------------------------
// Plane
// ----------------------------------------------------------------

drm_core::Plane::Plane(uint32_t id)
	:drm_core::ModeObject { ObjectType::plane, id } {
}

// ----------------------------------------------------------------
// Connector
// ----------------------------------------------------------------

drm_core::Connector::Connector(uint32_t id)
	:drm_core::ModeObject { ObjectType::connector, id } {
	_currentEncoder = nullptr;
	_connectorType = 0;
}

const std::vector<drm_mode_modeinfo> &drm_core::Connector::modeList() {
	return _modeList;
}

void drm_core::Connector::setModeList(std::vector<drm_mode_modeinfo> mode_list) {
	_modeList = mode_list;
}
	
void drm_core::Connector::setCurrentStatus(uint32_t status) {
	_currentStatus = status;
}
	
void drm_core::Connector::setCurrentEncoder(drm_core::Encoder *encoder) {
	_currentEncoder = encoder;
}
	
drm_core::Encoder *drm_core::Connector::currentEncoder() {
	return _currentEncoder;
}

uint32_t drm_core::Connector::getCurrentStatus() {
	return _currentStatus;
}
	
void drm_core::Connector::setupPossibleEncoders(std::vector<drm_core::Encoder *> encoders) {
	_possibleEncoders = encoders;
}

const std::vector<drm_core::Encoder *> &drm_core::Connector::getPossibleEncoders() {
	return _possibleEncoders;
}
	
void drm_core::Connector::setupPhysicalDimensions(uint32_t width, uint32_t height) {
	_physicalWidth = width;
	_physicalHeight = height;
}

uint32_t drm_core::Connector::getPhysicalWidth() {
	return _physicalWidth;
}

uint32_t drm_core::Connector::getPhysicalHeight() {
	return _physicalHeight;
}

void drm_core::Connector::setupSubpixel(uint32_t subpixel) {
	_subpixel = subpixel;
}

uint32_t drm_core::Connector::getSubpixel() {
	return _subpixel;
}

uint32_t drm_core::Connector::connectorType() {
	return _connectorType;
}

// ----------------------------------------------------------------
// Blob
// ----------------------------------------------------------------

size_t drm_core::Blob::size() {
	return _data.size();
}
	
const void *drm_core::Blob::data() {
	return _data.data();
}

// ----------------------------------------------------------------
// File
// ----------------------------------------------------------------

void drm_core::File::attachFrameBuffer(std::shared_ptr<drm_core::FrameBuffer> frame_buffer) {
	_frameBuffers.push_back(frame_buffer);
}
	
const std::vector<std::shared_ptr<drm_core::FrameBuffer>> &drm_core::File::getFrameBuffers() {
	return _frameBuffers;
}
	
uint32_t drm_core::File::createHandle(std::shared_ptr<BufferObject> bo) {
	auto handle = _allocator.allocate();
	_buffers.insert({handle, bo});
	return handle;
}
	
drm_core::BufferObject *drm_core::File::resolveHandle(uint32_t handle) {
	auto it = _buffers.find(handle);
	if(it == _buffers.end())
		return nullptr;
	return it->second.get();
};

async::result<size_t> drm_core::File::read(std::shared_ptr<void> object, void *buffer, size_t length) {
	throw std::runtime_error("read() not implemented");
}

COFIBER_ROUTINE(async::result<protocols::fs::AccessMemoryResult>, drm_core::File::accessMemory(std::shared_ptr<void> object,
		uint64_t offset, size_t), ([=] {
	auto self = std::static_pointer_cast<drm_core::File>(object);
	auto mapping = self->_device->findMapping(offset);
	auto mem = mapping.second->getMemory();
	COFIBER_RETURN(std::make_pair(mem.first, mem.second + (offset - mapping.first)));
}))

COFIBER_ROUTINE(async::result<void>, drm_core::File::ioctl(std::shared_ptr<void> object, managarm::fs::CntRequest req,
		helix::UniqueLane conversation), ([object = std::move(object), req = std::move(req),
		conversation = std::move(conversation)] {
	auto self = std::static_pointer_cast<drm_core::File>(object);
	if(req.command() == DRM_IOCTL_GET_CAP) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;
		
		if(req.drm_capability() == DRM_CAP_DUMB_BUFFER) {
			resp.set_drm_value(1);
			resp.set_error(managarm::fs::Errors::SUCCESS);
		}else{
			resp.set_drm_value(0);
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
		}
		
		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_GETRESOURCES) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		auto &crtcs = self->_device->getCrtcs();
		for(int i = 0; i < crtcs.size(); i++) {
			resp.add_drm_crtc_ids(crtcs[i]->id());
		}
			
		auto &encoders = self->_device->getEncoders();
		for(int i = 0; i < encoders.size(); i++) {
			resp.add_drm_encoder_ids(encoders[i]->id());
		}
	
		auto &connectors = self->_device->getConnectors();
		for(int i = 0; i < connectors.size(); i++) {
			resp.add_drm_connector_ids(connectors[i]->id());
		}
		
		auto &fbs = self->getFrameBuffers();
		for(int i = 0; i < fbs.size(); i++) {
			resp.add_drm_fb_ids(fbs[i]->id());
		}
	
		resp.set_drm_min_width(self->_device->getMinWidth());
		resp.set_drm_max_width(self->_device->getMaxWidth());
		resp.set_drm_min_height(self->_device->getMinHeight());
		resp.set_drm_max_height(self->_device->getMaxHeight());
		resp.set_error(managarm::fs::Errors::SUCCESS);
	
		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_GETCONNECTOR) {
		helix::SendBuffer send_resp;
		helix::SendBuffer send_list;
		managarm::fs::SvrResponse resp;
	
		auto obj = self->_device->findObject(req.drm_connector_id());
		assert(obj);
		auto conn = obj->asConnector();
		assert(conn);
		
		auto psbl_enc = conn->getPossibleEncoders();
		for(int i = 0; i < psbl_enc.size(); i++) { 
			resp.add_drm_encoders(psbl_enc[i]->id());
		}

		resp.set_drm_encoder_id(conn->currentEncoder()->id());
		resp.set_drm_connector_type(conn->connectorType());
		resp.set_drm_connector_type_id(0);
		resp.set_drm_connection(conn->getCurrentStatus()); // DRM_MODE_CONNECTED
		resp.set_drm_mm_width(conn->getPhysicalWidth());
		resp.set_drm_mm_height(conn->getPhysicalHeight());
		resp.set_drm_subpixel(conn->getSubpixel());
		resp.set_drm_num_modes(conn->modeList().size());
		resp.set_error(managarm::fs::Errors::SUCCESS);
	
		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
			helix::action(&send_list, conn->modeList().data(),
					conn->modeList().size() * sizeof(drm_mode_modeinfo)));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_list.error());
	}else if(req.command() == DRM_IOCTL_MODE_GETENCODER) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		resp.set_drm_encoder_type(0);

		auto obj = self->_device->findObject(req.drm_encoder_id());
		assert(obj);
		auto enc = obj->asEncoder();
		assert(enc);
		resp.set_drm_crtc_id(enc->currentCrtc()->id());
		
		uint32_t crtc_mask = 0;
		for(auto crtc : enc->getPossibleCrtcs()) {
			crtc_mask |= 1 << crtc->index;
		}
		resp.set_drm_possible_crtcs(crtc_mask);
		
		uint32_t clone_mask = 0;
		for(auto clone : enc->getPossibleClones()) {
			clone_mask |= 1 << clone->index;
		}
		resp.set_drm_possible_clones(clone_mask);

		resp.set_error(managarm::fs::Errors::SUCCESS);
	
		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_CREATE_DUMB) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		auto pair = self->_device->createDumb(req.drm_width(), req.drm_height(), req.drm_bpp());
		auto handle = self->createHandle(pair.first);
		resp.set_drm_handle(handle);

		resp.set_drm_pitch(pair.second);
		resp.set_drm_size(pair.first->getSize());
		resp.set_error(managarm::fs::Errors::SUCCESS);
	
		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_ADDFB) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		auto bo = self->resolveHandle(req.drm_handle());
		assert(bo);
		auto buffer = bo->sharedBufferObject();
		
		auto fb = self->_device->createFrameBuffer(buffer, req.drm_width(), req.drm_height(),
				req.drm_bpp(), req.drm_pitch());
		self->attachFrameBuffer(fb);
		resp.set_drm_fb_id(fb->id());
		resp.set_error(managarm::fs::Errors::SUCCESS);
	
		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_MAP_DUMB) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		auto bo = self->resolveHandle(req.drm_handle());
		assert(bo);
		auto buffer = bo->sharedBufferObject();
	
		resp.set_drm_offset(buffer->getMapping());
		resp.set_error(managarm::fs::Errors::SUCCESS);
	
		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_GETCRTC) {
		helix::SendBuffer send_resp;
		helix::SendBuffer send_mode;
		managarm::fs::SvrResponse resp;

		auto obj = self->_device->findObject(req.drm_crtc_id());
		assert(obj);
		auto crtc = obj->asCrtc();
		assert(crtc);

		drm_mode_modeinfo mode_info;
		if(crtc->currentMode()) {
			/* TODO: Set x, y, fb_id, gamma_size */
			memcpy(&mode_info, crtc->currentMode()->data(), sizeof(drm_mode_modeinfo));
			resp.set_drm_mode_valid(1);
		}else{
			memset(&mode_info, 0, sizeof(drm_mode_modeinfo));
			resp.set_drm_mode_valid(0);
		}
			
		resp.set_error(managarm::fs::Errors::SUCCESS);
	
		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
			helix::action(&send_mode, &mode_info, sizeof(drm_mode_modeinfo)));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_mode.error());
	}else if(req.command() == DRM_IOCTL_MODE_SETCRTC) {
		std::vector<char> mode_buffer;
		mode_buffer.resize(sizeof(drm_mode_modeinfo));

		helix::RecvBuffer recv_buffer;
		auto &&buff = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&recv_buffer, mode_buffer.data(), sizeof(drm_mode_modeinfo)));
		COFIBER_AWAIT buff.async_wait();
		HEL_CHECK(recv_buffer.error());
		
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;
	
		auto config = self->_device->createConfiguration();
	
		auto obj = self->_device->findObject(req.drm_crtc_id());
		assert(obj);
		auto crtc = obj->asCrtc();
		assert(crtc);
	
		std::vector<drm_core::Assignment> assignments;
		if(req.drm_mode_valid()) {
			auto mode_blob = std::make_shared<Blob>(std::move(mode_buffer));
			assignments.push_back(Assignment{ 
				crtc,
				&self->_device->modeIdProperty,
				0,
				nullptr,
				mode_blob
			});
			
			auto id = req.drm_fb_id();
			auto fb = self->_device->findObject(id);
			assert(fb);
			assignments.push_back(Assignment{ 
				crtc->primaryPlane(),
				&self->_device->fbIdProperty, 
				0,
				fb,
				nullptr
			});
		}else{
			std::vector<drm_core::Assignment> assignments;
			assignments.push_back(Assignment{ 
				crtc,
				&self->_device->modeIdProperty,
				0,
				nullptr,
				nullptr
			});
		}

		auto valid = config->capture(assignments);
		assert(valid);
		config->commit();
			
		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else{
		throw std::runtime_error("Unknown ioctl() with ID" + std::to_string(req.command()));
	}
	COFIBER_RETURN();
}))

constexpr auto fileOperations = protocols::fs::FileOperations{}
	.withRead(&drm_core::File::read)
	.withAccessMemory(&drm_core::File::accessMemory)
	.withIoctl(&drm_core::File::ioctl);

COFIBER_ROUTINE(cofiber::no_future, serveDevice(std::shared_ptr<drm_core::Device> device,
		helix::UniqueLane p), ([device = std::move(device), lane = std::move(p)] {
	std::cout << "unix device: Connection" << std::endl;

	while(true) {
		helix::Accept accept;
		helix::RecvInline recv_req;

		auto &&header = helix::submitAsync(lane, helix::Dispatcher::global(),
				helix::action(&accept, kHelItemAncillary),
				helix::action(&recv_req));
		COFIBER_AWAIT header.async_wait();
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());
		
		auto conversation = accept.descriptor();
		managarm::fs::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());
		if(req.req_type() == managarm::fs::CntReqType::DEV_OPEN) {
			helix::SendBuffer send_resp;
			helix::PushDescriptor push_node;
			
			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			auto file = std::make_shared<drm_core::File>(device);
			protocols::fs::servePassthrough(std::move(local_lane), file,
					&fileOperations);

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&push_node, remote_lane));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_node.error());
		}else{
			throw std::runtime_error("Invalid request in serveDevice()");
		}
	}
}))

// ----------------------------------------------------------------
// GfxDevice.
// ----------------------------------------------------------------

GfxDevice::GfxDevice(helix::UniqueDescriptor video_ram, void* frame_buffer)
: _videoRam{std::move(video_ram)}, _vramAllocator{24, 12}, _frameBuffer{frame_buffer} {
	uintptr_t ports[] = { 0x01CE, 0x01CF, 0x01D0 };
	HelHandle handle;
	HEL_CHECK(helAccessIo(ports, 3, &handle));
	HEL_CHECK(helEnableIo(handle));
	
	_operational = arch::global_io;
}

COFIBER_ROUTINE(cofiber::no_future, GfxDevice::initialize(), ([=] {
	_operational.store(regs::index, (uint16_t)RegisterIndex::id);
	auto version = _operational.load(regs::data); 
	if(version < 0xB0C2) {
		std::cout << "gfx/bochs: Device version 0x" << std::hex << version << std::dec
				<< " may be unsupported!" << std::endl;
	}

	_theCrtc = std::make_shared<Crtc>(this);
	_theEncoder = std::make_shared<Encoder>(this);
	_theConnector = std::make_shared<Connector>(this);
	_primaryPlane = std::make_shared<Plane>(this);
	
	registerObject(_theCrtc);
	registerObject(_theEncoder);
	registerObject(_theConnector);
	registerObject(_primaryPlane);
	
	_theEncoder->setCurrentCrtc(_theCrtc.get());
	_theConnector->setCurrentEncoder(_theEncoder.get());
	_theConnector->setCurrentStatus(1);
	_theEncoder->setupPossibleCrtcs({_theCrtc.get()});
	_theEncoder->setupPossibleClones({_theEncoder.get()});

	setupCrtc(_theCrtc);
	setupEncoder(_theEncoder);
	attachConnector(_theConnector);

	drm_mode_modeinfo mode;
	mode.clock = 47185;
	mode.hdisplay = 1024;
	mode.hsync_start = 1024;
	mode.hsync_end = 1024;
	mode.htotal = 1024;
	mode.hskew = 0;
	mode.vdisplay = 768;
	mode.vsync_start = 768;
	mode.vsync_end = 768;
	mode.vtotal = 768;
	mode.vscan = 0;
	mode.vrefresh = 60;
	mode.flags = 0;
	mode.type = 0;
	memcpy(&mode.name, "1024x768", 8);
	std::vector<drm_mode_modeinfo> mode_list;
	mode_list.push_back(mode);
	_theConnector->setModeList(mode_list);
	
	setupMinDimensions(640, 480);
	setupMaxDimensions(1024, 768);
		
	_theConnector->setupPhysicalDimensions(306, 230);
	_theConnector->setupSubpixel(0);
}))
	
std::unique_ptr<drm_core::Configuration> GfxDevice::createConfiguration() {
	return std::make_unique<Configuration>(this);
}

std::shared_ptr<drm_core::FrameBuffer> GfxDevice::createFrameBuffer(std::shared_ptr<drm_core::BufferObject> base_bo,
		uint32_t width, uint32_t height, uint32_t format, uint32_t pitch) {
	auto bo = std::static_pointer_cast<GfxDevice::BufferObject>(base_bo);
		
	assert(pitch % 4 == 0);
	auto pixel_pitch = pitch / 4;
	
	assert(pixel_pitch >= width);
	assert(bo->getAlignment() % pitch == 0);
	assert(bo->getSize() >= pitch * height);

	auto fb = std::make_shared<FrameBuffer>(this, bo, pixel_pitch);
	registerObject(fb);
	return fb;
}

std::pair<std::shared_ptr<drm_backend::BufferObject>, uint32_t>
GfxDevice::createDumb(uint32_t width, uint32_t height, uint32_t bpp) {
	assert(bpp == 32);
	unsigned int page_size = 4096;
	unsigned int bytes_pp = bpp / 8;

	// Buffers need to be aligned to lcm(pitch, page size). Here we compute a pitch that
	// minimizes the effective size (= data size + alignment) of the buffer.
	// avdgrinten: I'm not sure if there is a closed form expression for that, so we
	// just perform a brute-force search. Stop once the pitch is so big that no improvement
	// to the alignment can decrease the buffer size.
	auto best_ppitch = width;
	auto best_esize = std::lcm(bytes_pp * width, page_size) + bytes_pp * width * height;
	auto best_waste = std::lcm(bytes_pp * width, page_size);
	for(auto ppitch = width; bytes_pp * (ppitch - width) * height < best_waste; ++ppitch) {
		auto esize = std::lcm(bytes_pp * ppitch, page_size) + bytes_pp * ppitch * height;
		if(esize < best_esize) {
			best_ppitch = ppitch;
			best_esize = esize;
			best_waste = std::lcm(bytes_pp * best_ppitch, page_size)
					+ bytes_pp * (best_ppitch - width) * height;
		}
	}

	// TODO: Once we support VRAM <-> RAM eviction, we do not need to
	// statically determine the alignment at buffer creation time.
	auto pitch = bytes_pp * best_ppitch;
	auto alignment = std::lcm(pitch, page_size);
	auto size = pitch * height;

	auto offset = _vramAllocator.allocate(alignment + size);
	auto buffer = std::make_shared<BufferObject>(this, alignment, size,
			offset, alignment - (offset % alignment));

	auto mapping = installMapping(buffer.get());
	buffer->setupMapping(mapping);
	return std::make_pair(buffer, pitch);
}

// ----------------------------------------------------------------
// GfxDevice::Configuration.
// ----------------------------------------------------------------

bool GfxDevice::Configuration::capture(std::vector<drm_core::Assignment> assignment) {
	for(auto &assign: assignment) {
		if(assign.property == &_device->srcWProperty) {
			_width = assign.intValue;
		}else if(assign.property == &_device->srcHProperty) {
			_height = assign.intValue;
		}else if(assign.property == &_device->fbIdProperty) {
			auto fb = assign.objectValue->asFrameBuffer();
			if(!fb)
				return false;
			_fb = static_cast<GfxDevice::FrameBuffer *>(fb);
		}else if(assign.property == &_device->modeIdProperty) {
			_mode = assign.blobValue; 
			if(_mode) {
				drm_mode_modeinfo mode_info;
				memcpy(&mode_info, _mode->data(), sizeof(drm_mode_modeinfo));
				_height = mode_info.vdisplay;
				_width = mode_info.hdisplay;
			}
		}else{
			return false;
		}
	}

		
	if(_mode) {
		if(_width <= 0 || _height <= 0 || _width > 1024 || _height > 768)
			return false;
		if(!_fb)
			return false;
	}
	return true;	
}

void GfxDevice::Configuration::dispose() {

}

void GfxDevice::Configuration::commit() {
	drm_mode_modeinfo last_mode;
	memset(&last_mode, 0, sizeof(drm_mode_modeinfo));

	if(_device->_theCrtc->currentMode())
		memcpy(&last_mode, _device->_theCrtc->currentMode()->data(), sizeof(drm_mode_modeinfo));
	
	auto switch_mode = last_mode.hdisplay != _width || last_mode.vdisplay != _height;

	_device->_theCrtc->setCurrentMode(_mode);

	if(_mode) {
		if(switch_mode) {
			// The resolution registers must be written while the device is disabled.
			_device->_operational.store(regs::index, (uint16_t)RegisterIndex::enable);
			_device->_operational.store(regs::data, enable_bits::noMemClear | enable_bits::lfb);

			_device->_operational.store(regs::index, (uint16_t)RegisterIndex::resX);
			_device->_operational.store(regs::data, _width);
			_device->_operational.store(regs::index, (uint16_t)RegisterIndex::resY);
			_device->_operational.store(regs::data, _height);
			_device->_operational.store(regs::index, (uint16_t)RegisterIndex::bpp);
			_device->_operational.store(regs::data, 32);
			
			_device->_operational.store(regs::index, (uint16_t)RegisterIndex::enable);
			_device->_operational.store(regs::data, enable_bits::enable 
					| enable_bits::noMemClear | enable_bits::lfb);
			
		}

		// We do not have to write the virtual height.
		_device->_operational.store(regs::index, (uint16_t)RegisterIndex::virtWidth);
		_device->_operational.store(regs::data, _fb->getPixelPitch());

		// The offset registers have to be written while the device is enabled!
		assert(!(_fb->getBufferObject()->getAddress() % (_fb->getPixelPitch() * 4)));
		_device->_operational.store(regs::index, (uint16_t)RegisterIndex::offX);
		_device->_operational.store(regs::data, 0);	
		_device->_operational.store(regs::index, (uint16_t)RegisterIndex::offY);
		_device->_operational.store(regs::data, _fb->getBufferObject()->getAddress()
				/ (_fb->getPixelPitch() * 4));
	}else{
		_device->_operational.store(regs::index, (uint16_t)RegisterIndex::enable);
		_device->_operational.store(regs::data, enable_bits::noMemClear | enable_bits::lfb);
	}
}

// ----------------------------------------------------------------
// GfxDevice::Connector.
// ----------------------------------------------------------------

GfxDevice::Connector::Connector(GfxDevice *device)
	: drm_core::Connector { device->allocator.allocate() } {
	_encoders.push_back(device->_theEncoder.get());
}

// ----------------------------------------------------------------
// GfxDevice::Encoder.
// ----------------------------------------------------------------

GfxDevice::Encoder::Encoder(GfxDevice *device)
	:drm_core::Encoder { device->allocator.allocate() } {
}

// ----------------------------------------------------------------
// GfxDevice::Crtc.
// ----------------------------------------------------------------

GfxDevice::Crtc::Crtc(GfxDevice *device)
	:drm_core::Crtc { device->allocator.allocate() } {
	_device = device;
}

drm_core::Plane *GfxDevice::Crtc::primaryPlane() {
	return _device->_primaryPlane.get(); 
}

// ----------------------------------------------------------------
// GfxDevice::FrameBuffer.
// ----------------------------------------------------------------

GfxDevice::FrameBuffer::FrameBuffer(GfxDevice *device,
		std::shared_ptr<GfxDevice::BufferObject> bo, uint32_t pixel_pitch)
: drm_backend::FrameBuffer { device->allocator.allocate() } {
	_bo = bo;
	_pixelPitch = pixel_pitch;
}

GfxDevice::BufferObject *GfxDevice::FrameBuffer::getBufferObject() {
	return _bo.get();
}

uint32_t GfxDevice::FrameBuffer::getPixelPitch() {
	return _pixelPitch;
}

// ----------------------------------------------------------------
// GfxDevice: Plane.
// ----------------------------------------------------------------

GfxDevice::Plane::Plane(GfxDevice *device)
	:drm_core::Plane { device->allocator.allocate() } {
}

// ----------------------------------------------------------------
// GfxDevice: BufferObject.
// ----------------------------------------------------------------

std::shared_ptr<drm_core::BufferObject> GfxDevice::BufferObject::sharedBufferObject() {
	return this->shared_from_this();
}

size_t GfxDevice::BufferObject::getSize() {
	return _size;
}
	
std::pair<helix::BorrowedDescriptor, uint64_t> GfxDevice::BufferObject::getMemory() {
	return std::make_pair(helix::BorrowedDescriptor(_device->_videoRam),
			_offset + _displacement);
}

size_t GfxDevice::BufferObject::getAlignment() {
	return _alignment;
}

uintptr_t GfxDevice::BufferObject::getAddress() {
	return _offset + _displacement;
}

// ----------------------------------------------------------------
// Freestanding PCI discovery functions.
// ----------------------------------------------------------------

COFIBER_ROUTINE(cofiber::no_future, bindController(mbus::Entity entity), ([=] {
	protocols::hw::Device pci_device(COFIBER_AWAIT entity.bind());
	auto info = COFIBER_AWAIT pci_device.getPciInfo();
	assert(info.barInfo[0].ioType == protocols::hw::IoType::kIoTypeMemory);
	auto bar = COFIBER_AWAIT pci_device.accessBar(0);
	
	void *actual_pointer;
	HEL_CHECK(helMapMemory(bar.getHandle(), kHelNullHandle, nullptr,
			0, info.barInfo[0].length, kHelMapReadWrite | kHelMapShareAtFork, &actual_pointer));

	auto gfx_device = std::make_shared<GfxDevice>(std::move(bar), actual_pointer);
	gfx_device->initialize();

	// Create an mbus object for the device.
	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();
	
	mbus::Properties descriptor{
		{"unix.devtype", mbus::StringItem{"block"}},
		{"unix.devname", mbus::StringItem{"card0"}}
	};

	auto handler = mbus::ObjectHandler{}
	.withBind([=] () -> async::result<helix::UniqueDescriptor> {
		helix::UniqueLane local_lane, remote_lane;
		std::tie(local_lane, remote_lane) = helix::createStream();
		serveDevice(gfx_device, std::move(local_lane));

		async::promise<helix::UniqueDescriptor> promise;
		promise.set_value(std::move(remote_lane));
		return promise.async_get();
	});

	COFIBER_AWAIT root.createObject("gfx_bochs", descriptor, std::move(handler));
}))

COFIBER_ROUTINE(cofiber::no_future, observeControllers(), ([] {
	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("pci-vendor", "1234")
	});

	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties properties) {
		std::cout << "gfx/bochs: Detected device" << std::endl;
		bindController(std::move(entity));
	});

	COFIBER_AWAIT root.linkObserver(std::move(filter), std::move(handler));
}))

int main() {
	printf("gfx/bochs: Starting driver\n");
	
	observeControllers();

	while(true)
		helix::Dispatcher::global().dispatch();
	
	return 0;
}

