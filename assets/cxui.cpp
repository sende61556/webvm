// Copyright 2019-2024 Leaning Technologies Ltd.

#include <cheerp/coroutine.h>
#include "coredata.h"
#include "devices/vgaout.h"
#include "cxuibase.h"
#include "workerclock.h"
#include "cxuidevices.h"

// This file should contain the public interface for the CheerpX system

CheerpX::Device* CheerpXBase::getDeviceById(uint32_t id)
{
	for (CheerpX::Device* d: devices)
	{
		if (d->devId == id)
			return d;
	}
	return nullptr;
}

namespace [[cheerp::genericjs]] client
{
	struct FloppyConfiguration: public Object
	{
		CheerpX::BlockDevice* get_dev();
		uint32_t get_size();
		// TODO: Add option to specify floppy id
	};
	struct DiskConfiguration: public Object
	{
		CheerpX::BlockDevice* get_dev();
		const client::String& get_type();
		uint32_t get_id();
	};
	struct SystemConfiguration: public Object
	{
		uint32_t get_MhZ();
		CheerpX::BlockDevice* get_bios();
		CheerpX::BlockDevice* get_vgaBios();
		TArray<FloppyConfiguration*>* get_floppies();
		TArray<DiskConfiguration*>* get_disks();
		uint32_t get_mem();
	};
	client::Uint8Array* HEAP8;
	client::Uint16Array* HEAP16;
	client::Int32Array* HEAP32;
	struct PromiseCallbacks: public Object
	{
		client::EventListener* get_fullfill();
		client::EventListener* get_reject();
	};
	Promise<_Any*>* import(const String&);
	namespace CheerpX {
	struct System: public Object {
		// NOTE: Only pass jsexported types!
		template<typename T>
		static client::Object* wrap(T*);
	};
	}
}

namespace [[cheerp::genericjs]] CheerpX
{

class [[cheerp::genericjs]] [[cheerp::jsexport_unsafe]] System: public CheerpXBase
{
private:
	void handleCoreMessage(client::CoreMessage* m);
	// 'index' represent the attachment point of the disk in a controller
	client::Object* createIdeDiskMsg(CORE_DISK_TYPE type, uint32_t index, uint32_t id, uint32_t imageLen);
	static client::Object* createFloppyDiskMsg(uint32_t index, uint32_t imageLen);
	static client::Object* swapFloppyMsg(uint32_t index, uint32_t id, uint32_t imageLen, bool isWriteProtected);
	static Thread runImpl(System* t, client::SystemConfiguration* conf);
	static Thread runIOReadRequest(System* t, uint32_t id, uint32_t start, uint32_t len, uint32_t ioTransaction, uint32_t bufOffset);
	static Thread runIOWriteRequest(System* t, uint32_t id, uint32_t start, uint32_t len, uint32_t ioTransaction, uint32_t bufOffset);
	void handleKeyDown(client::KeyboardEvent* ev);
	void handleKeyUp(client::KeyboardEvent* ev);
	Task<client::Object*> cheerpOSInit();
public:
	System();
	~System()
	{
	}
	static client::Promise<client::_Any*>* create();
	void run(client::SystemConfiguration* conf)
	{
		runImpl(this, conf);
	}
	void createHud()
	{
		createHudImpl();
	}
	// Hack to allow us to wrap the object returned to js by the create promise
	// Used together with the client declaration below
	static client::Object* wrap(client::Object* o)
	{
		return o;
	}
};

}

[[cheerp::genericjs]] client::String* getCheerpXUrl()
{
	client::TArray<client::String*>* tmp = new client::TArray<client::String*>();
	__asm__("try{throw new Error();}catch(e){%0.push(e.stack);}" : : "r"(tmp));
	client::String* stackStr = (*tmp)[0];
	int cxStart = stackStr->indexOf("/" CXFILE);
	assert(cxStart > 0);
	int httpStart = stackStr->lastIndexOf("http:", cxStart);
	int httpsStart = stackStr->lastIndexOf("https:", cxStart);
	int urlStart = httpStart > httpsStart ? httpStart : httpsStart;
	if (urlStart < 0)
		urlStart = stackStr->lastIndexOf("chrome-extension:", cxStart);
	assert(urlStart > 0);
	return stackStr->substring(urlStart, cxStart+1);
}


CheerpXBase::CHEERP_OS_STATE CheerpXBase::cheerpOSState = NOT_LOADED;
CheerpXBase* CheerpXBase::waitingForCheerpOSList;
client::NetworkConf* CheerpXBase::tsNetworkConf = nullptr;
std::vector<CheerpX::Device*> CheerpXBase::devices;

void CheerpXBase::handleCheerpOSLoadEvent()
{
	if(cheerpOSState == LOADING_1)
		cheerpOSState = LOADING_2;
	else if(cheerpOSState == LOADING_2)
	{
		cheerpOSState = READY;
		CheerpXBase* cur = waitingForCheerpOSList;
		waitingForCheerpOSList = nullptr;
		while(cur)
		{
			cur->createCoreWorker();
			CheerpXBase* next = cur->next;
			cur->next = nullptr;
			cur = next;
		}
	}
}

void CheerpXBase::loadTailScale()
{
	client::String* baseUrl = getCheerpXUrl();
	client::String* tsNetworkUrl = baseUrl->concat("tun/tailscale_tun_auto.js");
	client::Promise<client::_Any*>* tsNetworkConfP = client::import(tsNetworkUrl);
	tsNetworkConfP->then(cheerp::Callback([](client::NetworkConf* o)
	{
		tsNetworkConf = o;
		handleCheerpOSLoadEvent();
	}));

}

client::Promise<client::_Any*>* CheerpXBase::loadCheerpOS()
{
	PromiseData p = createPromise();
	client::String* baseUrl = getCheerpXUrl();
	cheerpOSState = LOADING_1;
	client::String* cheerpOSUrl = baseUrl->concat("cheerpOS.js");
	client::HTMLScriptElement* s2 = (client::HTMLScriptElement*)client::document.createElement("script");
	s2->set_src(cheerpOSUrl);
	s2->set_onload(cheerp::Callback([p]() -> void
	{
		handleCheerpOSLoadEvent();
		((PromiseFullfiller)p.f)(nullptr);
	}));
	//TODO: CheerpOS should not be appended to head to make sure it's not accessible to users
	client::document.get_head()->appendChild(s2);
	return p.p;
}

CheerpXBase::CheerpXBase():next(nullptr),coreMessageHandler(nullptr),fullfillPromise(nullptr),rejectPromise(nullptr),
				jitErrorCallback(nullptr),core(nullptr),asyncPtrOffset(0),bridgeURL(nullptr),hudDiv(nullptr),statsDiv(nullptr),
				dbgCtxsDiv(nullptr),dbgControlDiv(nullptr),dbgCtxSelect(nullptr),dbgStartStopBtn(nullptr),dbgDisasDiv(nullptr),
				dbgDisasMode(nullptr),dbgDisasAddr(nullptr),dbgDisasBtn(nullptr),dbgDisasView(nullptr),jitBisectArea(nullptr),
				dbgCurCtx(nullptr)
{
}

CheerpX::System::System()
{
	coreMessageHandler = cheerp::Callback([this](client::MessageEvent<client::Object*>* e)
	{
		client::CoreMessage* m = (client::CoreMessage*)e->get_data();
		handleCoreMessage(m);
	});
}

void CheerpXBase::init(PromiseFullfiller f, PromiseRejecter r)
{
	fullfillPromise = f;
	rejectPromise = r;
	// First of all we need to load cheerpOS components if they are not loaded already
	if(cheerpOSState == NOT_LOADED)
		loadCheerpOS();
	if(cheerpOSState != READY)
		loadTailScale();
	if(cheerpOSState == READY)
		createCoreWorker();
	else
	{
		next = waitingForCheerpOSList;
		waitingForCheerpOSList = this;
	}
}

void CheerpXBase::handleCoreMessageBase(client::CoreMessage* m)
{
	if(m->get_type() == CORE_INIT)
	{
		// Ok, we now have (some) internal data about the core thread
		client::HEAP8 = new client::Uint8Array(m->get_buffer());
		client::HEAP16 = new client::Uint16Array(m->get_buffer());
		client::HEAP32 = new client::Int32Array(m->get_buffer());
		asyncPtrOffset = m->get_asyncPtrOffset() >> 2;
		bool needWorkerClock = m->get_startRealTime() >= 0;
		if (needWorkerClock)
		{
			// Start the timer worker
			auto StartTimerWorker = [this, m]() -> Thread
			{
				client::String* cxCoreUrl = getCheerpXUrl();
				client::Response* r = co_await *client::fetch(cxCoreUrl->concat("workerclock.js"));
				client::String* code = co_await *r->text();
				client::Blob* b = new client::Blob(new client::Array(code));
				client::String* bUrl = client::URL.createObjectURL(b);
				client::Worker* workerClock = new client::Worker(bUrl);
				client::MessageChannel* c = new client::MessageChannel();
				client::MessagePort* corePort = c->get_port1();
				// Build a channel between the worker clock and the core
				client::Object* ret = nullptr;
				__asm__("{type:%1, value: %2}" : "=r"(ret) : "r"(CORE_TIMER_PORT), "r"(corePort));
				core->postMessage(ret, new client::Array(corePort));
				workerClock->set_onmessage(cheerp::Callback([this]()
				{
					cheerpOsInitImpl();
				}));
				client::MessagePort* timerPort = c->get_port2();
				client::Object* tmp;
				__asm__("{kind:%1, buffer:%2, basePtr:%3, startRealTime:%4, port: %5}" : "=r"(tmp) : "r"(INIT_MEMORY), "r"(m->get_buffer()), "r"(m->get_asyncPtrOffset()), "r"(m->get_startRealTime()), "r"(timerPort));
				workerClock->postMessage(tmp, new client::Array(timerPort));
			};
			StartTimerWorker();
		}
		else
		{
			cheerpOsInitImpl();
		}
	}
	else if(m->get_type() == CORE_INIT_RETRY)
	{
		coreWorker("cxcore-no-return-call.js", CORE_INIT_FAILED);
	}
	else if(m->get_type() == CORE_INIT_FAILED)
	{
		client::String* msg = new client::String("CheerpX initialization failed: ");
		msg = msg->concat(m->get_value<client::String*>());
		rejectPromise(msg);
		rejectPromise = nullptr;
	}
	else if(m->get_type() == CORE_COMPILE_WASM_REQUEST)
	{
		uint32_t moduleBufferStart = m->get_start();
		uint32_t moduleBufferLength = m->get_len();
		client::Uint8Array* buf = client::HEAP8->subarray(moduleBufferStart, moduleBufferStart+moduleBufferLength);
		CORE_MESSAGE replyType = m ->get_replyType();
#ifdef MODULE_TIMINGS
		double requestTime = m->get_requestTime();
		double compileStartTime = client::Date::now();
		client::WebAssembly::compile(buf)->then(cheerp::Callback([this,replyType,requestTime,compileStartTime,buf](client::Object* wasmModule)
#else
		client::WebAssembly::compile(buf)->then(cheerp::Callback([this,replyType,buf](client::Object* wasmModule)
#endif
		{
			client::Object* ret = nullptr;
#ifdef MODULE_TIMINGS
			__asm__("{type:%1, wasmModule:%2, requestTime:%3, compileStartTime:%4, compileEndTime:%5, fileSize:%6}" : "=r"(ret) : "r"(replyType), "r"(wasmModule),
													"r"(requestTime), "r"(compileStartTime), "r"(client::Date::now()), "r"(buf->get_length()));
#else
			__asm__("{type:%1, wasmModule:%2}" : "=r"(ret) : "r"(replyType), "r"(wasmModule));
#endif
			if(replyType == CORE_COMPILE_WASM_RESULT)
				(*client::HEAP32)[asyncPtrOffset + 5] = WASM_MODULE_COMPLETE;
			postMessage(ret, /*sendInterrupt*/true);
#if 0
			if(buf->get_length() > 18000000)
			{
				client::Blob* b = new client::Blob(new client::Array(buf));
				client::String* url = client::URL.createObjectURL(b);
				client::CoreMessage* ret = nullptr;
				__asm__("{type:%1, path:%2, value:%3}" : "=r"(ret) : "r"(DUMP_DATA), "r"(url), "r"(new client::String("big.wasm")));
				handleCoreMessageBase(ret);
			}
#endif
		}),
		cheerp::Callback([this,buf](client::String* s)
		{
			// Send a message to this same thread, to keep dump handling unified
			client::console.log(s);
			// Also let the user code handle this
			if(jitErrorCallback)
				jitErrorCallback(s);
			client::Blob* b = new client::Blob(new client::Array(new client::Uint8Array(buf)));
			client::String* url = client::URL.createObjectURL(b);
			client::CoreMessage* ret = nullptr;
			__asm__("{type:%1, path:%2, value:%3}" : "=r"(ret) : "r"(DUMP_DATA), "r"(url), "r"(new client::String("fail.wasm")));
			handleCoreMessageBase(ret);
		}));
	}
	else if(m->get_type() == CORE_HUD_GLOBAL_STAT)
	{
		uint32_t* linearPtr = __builtin_cheerp_make_regular<uint32_t>(client::HEAP32, m->get_intWrapper() >> 2);
		globalStats.emplace_back(statsDiv, m->get_statName(), linearPtr, m->get_statType());
	}
	else if(m->get_type() == CORE_HUD_ADD_CONTEXT)
	{
		ContextData* newCtx = new ContextData{m->get_ctxType(), m->get_value(), m->get_dbgState()};
		dbgCtxs.push_back(newCtx);
		updateContexts();
	}
	else if(m->get_type() == CORE_HUD_REMOVE_CONTEXT)
	{
		ContextData* newCtx = new ContextData{m->get_ctxType(), m->get_value(), m->get_dbgState()};
		auto it = std::remove_if(dbgCtxs.begin(), dbgCtxs.end(), [m](const ContextData* c)
		{
			return c->ctxType == m->get_ctxType() && c->ctxId == m->get_value();
		});
		if(it != dbgCtxs.end())
		{
			dbgCtxs.erase(it, dbgCtxs.end());
			updateContexts();
		}
	}
	else if(m->get_type() == CORE_HUD_UPDATE_CONTEXT)
	{
		ContextData* c = getCtxDataForId(m->get_ctxType(), m->get_value());
		if(c)
		{
			c->state = m->get_dbgState();
			selectContext(dbgCurCtx);
		}
	}
	else if(m->get_type() == CORE_DBG_DISAS_RESULT)
	{
		dbgDisasView->set_textContent(m->get_text());
	}
	else if(m->get_type() == CORE_JIT_GET_CUR_TRACES)
	{
		client::String* text = new client::String();
		auto& traces = *m->get_traces();
		for(int i = 0; i < traces.get_length(); ++i)
		{
			text = text->concat((new client::Number(traces[i]))->toString(16))->concat("\n");
		}
		text = text->trim();
		jitBisectArea->set_value(text);
	}
	else if(m->get_type() == DUMP_DATA)
	{
		client::String* str = m->get_path();
		uint32_t counter = m->get_value();
		client::HTMLElement* a = client::document.createElement("a");
		a->setAttribute("href", str);
		a->setAttribute("download", m->get_value<client::String*>());
		a->click();
		client::URL::revokeObjectURL(str);
	}
	else
	{
		__asm__("debugger");
	}
}


void CheerpX::System::handleCoreMessage(client::CoreMessage* m)
{
	if(m->get_type() == CORE_START_VGA)
	{
		// Create the VGA renderer
		VGAShared* vgaShared = __builtin_cheerp_make_regular<VGAShared>(new client::DataView(client::HEAP8->get_buffer()), m->get_vgaDevice());
		VGAOutput::initialize(__builtin_cheerp_make_regular<uint8_t>(client::HEAP8, m->get_vgaRamOffset()), *vgaShared);
		// Also start the keyboard handlers
		client::document.addEventListener("keydown", cheerp::Callback([this](client::KeyboardEvent* ev){handleKeyDown(ev);}));
		client::document.addEventListener("keyup", cheerp::Callback([this](client::KeyboardEvent* ev){handleKeyUp(ev);}));
	}
	else if(m->get_type() == CORE_VGA_MODE)
	{
		VGA_RENDER_MODE mode = (VGA_RENDER_MODE)m->get_value();
		VGAOutput::setRenderMode(mode);
	}
	else if(m->get_type() == CORE_VGA_SET_WIDTH)
	{
		VGAOutput::setWidth(m->get_value());
	}
	else if(m->get_type() == CORE_VGA_SET_HEIGHT)
	{
		VGAOutput::setHeight(m->get_value());
	}
	else if(m->get_type() == CORE_IO_READ_REQUEST)
	{
		runIOReadRequest(this, m->get_devId(), m->get_start(), m->get_len(), m->get_ioTransaction(), m->get_value());
	}
	else if(m->get_type() == CORE_IO_WRITE_REQUEST)
	{
		runIOWriteRequest(this, m->get_devId(), m->get_start(), m->get_len(), m->get_ioTransaction(), m->get_value());
	}
	else
		handleCoreMessageBase(m);
}

void CheerpXBase::createCoreWorker()
{
	if(bridgeURL != nullptr)
	{
		client::String* cxBridgeUrl = getCheerpXUrl()->concat("cxbridge.js");
		core = new client::Worker(cxBridgeUrl);
		// Before proceding we need to wait for the core worker to be initialized
		core->set_onmessage(coreMessageHandler);
	}
	else
	{
		coreWorker("cxcore.js", CORE_INIT_RETRY);
	}
}

Thread CheerpXBase::coreWorker(const client::String& coreFile, CORE_MESSAGE m)
{
	client::String* cxCoreUrl = getCheerpXUrl();
	client::Response* r = co_await *client::fetch(cxCoreUrl->concat(coreFile));
	client::String* code = co_await *r->text();
	client::String* wasmFile = coreFile.replace(".js", ".wasm");
	code = code->replace(wasmFile, cxCoreUrl->concat(wasmFile));
	code = code->concat("cxCoreInit.promise.then(function(){cxCoreInit();}).catch(function(e){postMessage({type:", m, ",value:e.toString()});})");
	client::Blob* b = new client::Blob(new client::Array(code));
	client::String* bUrl = client::URL.createObjectURL(b);
	core = new client::Worker(bUrl);
	core->set_onmessage(coreMessageHandler);
}

Thread CheerpXBase::cheerpOsInitImpl()
{
	client::Object* ret = co_await cheerpOSInit();
	// Everything is ready, fullfill the promise
	if(ret == nullptr)
	{
		rejectPromise("CheerpX initialization failed");
	}
	else
	{
		fullfillPromise(ret);
		fullfillPromise = nullptr;
	}
}

CheerpXBase::PromiseData CheerpXBase::createPromise()
{
	// Create the promise object, store the fullfill callback in a temporary object
	client::Promise<client::_Any*>* ret = nullptr;
	client::PromiseCallbacks* tmp = new client::PromiseCallbacks();
	__asm__("new Promise(function(f,r){%1.fullfill=f;%1.reject=r;});" : "=r"(ret) : "r"(tmp));
	return PromiseData{ret, tmp->get_fullfill(), tmp->get_reject()};
}

client::Promise<client::_Any*>* CheerpX::System::create()
{
	System* s = new System();
	PromiseData d = createPromise();
	s->init((PromiseFullfiller)d.f, (PromiseRejecter)d.r);
	return d.p;
}

client::Object* CheerpX::System::createIdeDiskMsg(CORE_DISK_TYPE type, uint32_t index, uint32_t id, uint32_t imageLen)
{
	client::Object* result;
	__asm__("{type:%1,diskType:%2,index:%3,devId:%4,len:%5}" : "=r"(result) : "r"(CORE_CREATE_IDE_DISK), "r"(type), "r"(index), "r"(id), "r"(imageLen));
	return result;
}

client::Object* CheerpX::System::createFloppyDiskMsg(uint32_t index, uint32_t imageLen)
{
	client::Object* result;
	__asm__("{type:%1,index:%2,len:%3}" : "=r"(result) : "r"(CORE_CREATE_FLOPPY_DISK), "r"(index), "r"(imageLen));
	return result;
}

client::Object* CheerpX::System::swapFloppyMsg(uint32_t index, uint32_t id, uint32_t imageLen, bool isWriteProtected)
{
	client::Object* result;
	__asm__("{type:%1,index:%2,devId:%3,len:%4,writeProtected:%5}" : "=r"(result) : "r"(CORE_SWAP_FLOPPY), "r"(index), "r"(id), "r"(imageLen), "r"(isWriteProtected));
	return result;
}

Thread CheerpX::System::runImpl(System* t, client::SystemConfiguration* conf)
{
	if (!conf->hasOwnProperty("bios") || !conf->hasOwnProperty("vgaBios"))
	{
		client::console.log("bios and vgaBios must be defined");
		co_await Suspender<uint32_t>();
	}
	CheerpX::BlockDevice* biosDevice = conf->get_bios();
	assert(biosDevice->type == CheerpX::Device::BLOCK);
	client::Uint8Array* biosData = new client::Uint8Array(biosDevice->length);
	co_await biosDevice->read(t, 0, biosDevice->length, biosData, 0);

	CheerpX::BlockDevice* vgaBiosDevice = conf->get_vgaBios();
	assert(vgaBiosDevice->type == CheerpX::Device::BLOCK);
	client::Uint8Array* vgaBiosData = new client::Uint8Array(vgaBiosDevice->length);
	co_await vgaBiosDevice->read(t, 0, vgaBiosDevice->length, vgaBiosData, 0);
	// Initialize an address space layout with the information in 'conf'
	client::Array* transferList = new client::Array();;
	client::Object* result = nullptr;
	uint32_t mhz = 0;
	if (conf->hasOwnProperty("MhZ"))
		mhz = conf->get_MhZ();
	__asm__("{type:%1,mhz:%2,mem:%3,bios:%4,vgaBios:%5}" : "=r"(result) : "r"(CORE_INIT_SYSTEM),
					"r"(mhz), "r"(conf->get_mem()), "r"(biosData), "r"(vgaBiosData));
	transferList->push(biosData->get_buffer());;
	transferList->push(vgaBiosData->get_buffer());;
	t->core->postMessage(result, transferList);
	auto HandleFloppyConf = [](System* t, uint32_t floppyIndex, client::FloppyConfiguration* floppyConf) -> Task<void>
	{
		if (floppyIndex > 1)
		{
			client::console.log("Invalid floppy id", floppyIndex);
			co_await Suspender<uint32_t>();
		}
		if (floppyConf->hasOwnProperty("dev"))
		{

			BlockDevice* flp = floppyConf->get_dev();
			assert(flp->type != CheerpX::Device::CHEERPOS);
			if (floppyConf->hasOwnProperty("size") && floppyConf->get_size()*1024 != flp->length)
			{
				client::console.log("Unexpected Floppy size");
				co_await Suspender<uint32_t>();
			}
			bool isWriteProtected = (co_await flp->getPermType() & 2) == 0;
			t->core->postMessage(System::createFloppyDiskMsg(floppyIndex, flp->length));
			t->core->postMessage(System::swapFloppyMsg(floppyIndex, flp->devId, flp->length, isWriteProtected));
		}
		if (floppyConf->hasOwnProperty("size"))
			t->core->postMessage(CheerpX::System::createFloppyDiskMsg(1, floppyConf->get_size() * 1024));
	};
	if(conf->hasOwnProperty("floppies") && client::Array::isArray(conf->get_floppies()))
	{
		const client::TArray<client::FloppyConfiguration*>& floppies = *conf->get_floppies();
		for(uint32_t i=0;i<floppies.get_length();i++)
		{
			co_await HandleFloppyConf(t, i, floppies[i]);
		}
	}
	if(conf->hasOwnProperty("disks") && client::Array::isArray(conf->get_disks()))
	{
		const client::TArray<client::DiskConfiguration*>& disks = *conf->get_disks();
		bool diskIds[2] = {false, false};
		for(uint32_t i=0;i<disks.get_length();i++)
		{
			BlockDevice* dev = disks[i]->get_dev();
			uint32_t diskId;
			if(disks[i]->hasOwnProperty("id"))
				diskId = disks[i]->get_id();
			else
				diskId = i;
			if(diskId > 1)
			{
				client::console.log("Invalid disk id", diskId);
				co_await Suspender<uint32_t>();
			}
			if(diskIds[diskId])
			{
				client::console.log("Overwriting disk id", diskId);
				co_await Suspender<uint32_t>();
			}
			diskIds[diskId] = true;
			CORE_DISK_TYPE type;
			if(disks[i]->get_type().localeCompare("ata")==0)
				type = DISK_HD;
			else if(disks[i]->get_type().localeCompare("atapi")==0)
				type = DISK_CD;
			else
			{
				client::console.log("Unknown disk type");
				co_await Suspender<uint32_t>();
			}
			t->core->postMessage(t->createIdeDiskMsg(type, diskId, dev->devId, dev->length));
		}
	}
	__asm__("{type:%1}" : "=r"(result) : "r"(CORE_START_SYSTEM));
	t->core->postMessage(result);
}

Thread CheerpX::System::runIOReadRequest(System* t, uint32_t id, uint32_t start, uint32_t len, uint32_t ioTransaction, uint32_t bufOffset)
{
	CheerpX::Device* dev = t->getDeviceById(id);
	assert(dev->type == Device::TYPE::BLOCK);
	CheerpX::BlockDevice* device = static_cast<CheerpX::BlockDevice*>(dev);
	uint32_t readBytes = co_await device->read(t, start, len, client::HEAP8, bufOffset);
	client::Object* result = nullptr;
	__asm__("{type:%1,ioTransaction:%2}" : "=r"(result) : "r"(CORE_IO_RESULT), "r"(ioTransaction));
	t->postMessage(result, /*sendInterrupt*/true);
}

Thread CheerpX::System::runIOWriteRequest(System* t, uint32_t id, uint32_t start, uint32_t len, uint32_t ioTransaction, uint32_t bufOffset)
{
	CheerpX::Device* dev = t->getDeviceById(id);
	assert(dev->type == Device::TYPE::BLOCK);
	CheerpX::BlockDevice* device = static_cast<CheerpX::BlockDevice*>(dev);
	int doneBytes = co_await device->write(t, start, len, client::HEAP8, bufOffset);
	client::Object* result = nullptr;
	__asm__("{type:%1,ioTransaction:%2}" : "=r"(result) : "r"(CORE_IO_RESULT), "r"(ioTransaction));
	t->postMessage(result, /*sendInterrupt*/true);
}

// Populate the queue using genericjs code, concurrently with the core using the data
// Send an IRQ 1 message and force the thread to stop by setting the flag
// TODO: This is not great, it would be much better to have a way to safely mark the IRQ from here
void CheerpX::System::handleKeyDown(client::KeyboardEvent* ev)
{
	// Do not handle Ctrl+Shift combos
	if(ev->get_ctrlKey() && ev->get_shiftKey())
		return;
	ev->preventDefault();
	client::Object* result = nullptr;
	__asm__("{type:%1,value:%2}" : "=r"(result) : "r"(CORE_QUEUE_KEYDOWN), "r"(ev->get_keyCode()));
	postMessage(result, /*sendInterrupt*/true);
}

void CheerpX::System::handleKeyUp(client::KeyboardEvent* ev)
{
	// Do not handle Ctrl+Shift combos
	if(ev->get_ctrlKey() && ev->get_shiftKey())
		return;
	ev->preventDefault();
	client::Object* result = nullptr;
	__asm__("{type:%1,value:%2}" : "=r"(result) : "r"(CORE_QUEUE_KEYUP), "r"(ev->get_keyCode()));
	postMessage(result, /*sendInterrupt*/true);
}

Task<client::Object*> CheerpX::System::cheerpOSInit()
{
	co_return client::CheerpX::System::wrap(this);
}

void CheerpXBase::postMessage(client::Object* msg, bool sendInterrupt)
{
	if(sendInterrupt)
		(*client::HEAP32)[asyncPtrOffset + 0] = -2;
	core->postMessage(msg);
}

void CheerpXBase::updateHud()
{
	for(HudGlobalStat& s: globalStats)
		s.update();
}

void CheerpXBase::sliceWidth(client::HTMLElement* e, const client::String& w)
{
	e->get_style()->set_width(w);
	e->get_style()->set_boxSizing("border-box");
}

CheerpXBase::ContextData* CheerpXBase::getCtxDataForId(CONTEXT_TYPE t, uint32_t i) const
{
	for(ContextData* c: dbgCtxs)
	{
		if(c->ctxType == t && c->ctxId == i)
			return c;
	}
	return nullptr;
}

CheerpXBase::ContextData* CheerpXBase::getCtxDataForName(client::String* n) const
{
	for(ContextData* c: dbgCtxs)
	{
		if(c->displayName->localeCompare(*n) == 0)
			return c;
	}
	return nullptr;
}

void CheerpXBase::createHudImpl()
{
	hudDiv = client::document.createElement("div");
	auto stopEvent = [](client::Event* e) { e->stopPropagation(); };
	hudDiv->addEventListener("keydown", cheerp::Callback(stopEvent));
	hudDiv->addEventListener("keyup", cheerp::Callback(stopEvent));
	hudDiv->addEventListener("keypress", cheerp::Callback(stopEvent));
	hudDiv->setAttribute("style", "position:absolute;width:25%;height:100%;top:0;right:0;overflow-y:scroll;");
	statsDiv = client::document.createElement("div");
	// This block will contain variuos global statistics
	appendHudBlock("Global stats", statsDiv);
	dbgCtxsDiv = client::document.createElement("div");
	// This block will contain the list of contexts that we can debug
	appendHudBlock("Debugger - Contexts", dbgCtxsDiv);
	dbgCtxSelect = (client::HTMLSelectElement*)client::document.createElement("select");
	dbgCtxsDiv->appendChild(dbgCtxSelect);
	sliceWidth(dbgCtxSelect, "50%");
	dbgControlDiv = client::document.createElement("div");
	// This block will contain the controls for starting/stopping/stepping a context
	appendHudBlock("Debugger - Control", dbgControlDiv);
	dbgStartStopBtn = (client::HTMLButtonElement*)client::document.createElement("button");
	sliceWidth(dbgStartStopBtn, "50%");
	dbgControlDiv->appendChild(dbgStartStopBtn);
	dbgDisasDiv = client::document.createElement("div");
	// This block will contain the disassembly/memory views
	appendHudBlock("Debugger - Disassembly", dbgDisasDiv);
	dbgDisasMode = (client::HTMLSelectElement*)client::document.createElement("select");
	dbgDisasMode->appendChild(createOption("16-bit", "0"));
	dbgDisasMode->appendChild(createOption("32-bit", "1"));
	dbgDisasMode->appendChild(createOption("Wasm (dump)", "2"));
	sliceWidth(dbgDisasMode, "20%");
	dbgDisasAddr = (client::HTMLInputElement*)client::document.createElement("input");
	sliceWidth(dbgDisasAddr, "20%");
	dbgDisasBtn = (client::HTMLButtonElement*)client::document.createElement("button");
	sliceWidth(dbgDisasBtn, "20%");
	dbgDisasBtn->set_textContent("Show");
	dbgDisasView = client::document.createElement("pre");
	dbgDisasDiv->appendChild(dbgDisasMode);
	dbgDisasDiv->appendChild(dbgDisasAddr);
	dbgDisasDiv->appendChild(dbgDisasBtn);
	dbgDisasDiv->appendChild(dbgDisasView);
	dbgDisasBtn->set_onclick(cheerp::Callback([this]()
	{
		assert(dbgCurCtx && dbgCurCtx->state == DBG_STOPPED);
		uint32_t selectedMode = client::parseInt(dbgDisasMode->get_value());
		client::String* addrStr = dbgDisasAddr->get_value();
		if(addrStr->get_length() == 0)
			return;
		uint32_t selectedAddr = client::parseInt(addrStr, 16);
		client::Object* result = nullptr;
		CORE_MESSAGE msg;
		if(selectedMode == 0)
			msg = CORE_DBG_DISAS_16;
		else if(selectedMode == 1)
			msg = CORE_DBG_DISAS_32;
		else if(selectedMode == 2)
			msg = CORE_DBG_DUMP_WASM;
		else
			return;
		__asm__("{type:%1,ctxType:%2,value:%3,addr:%4}" : "=r"(result) : "r"(msg), "r"(dbgCurCtx->ctxType), "r"(dbgCurCtx->ctxId), "r"(selectedAddr));
		postMessage(result, /*sendInterrupt*/true);
	}));
	client::Element* jitBisectDiv = client::document.createElement("div");
	appendHudBlock("JIT - Bisect", jitBisectDiv);
	jitBisectArea = (client::HTMLInputElement*)client::document.createElement("textarea");
	jitBisectSet = (client::HTMLButtonElement*)client::document.createElement("button");
	jitBisectCur = (client::HTMLButtonElement*)client::document.createElement("button");
	jitBisectDiv->appendChild(jitBisectArea);
	jitBisectDiv->appendChild(jitBisectSet);
	jitBisectDiv->appendChild(jitBisectCur);
	sliceWidth(jitBisectArea, "100%");
	sliceWidth(jitBisectSet, "50%");
	sliceWidth(jitBisectCur, "50%");
	jitBisectSet->set_textContent("Apply");
	jitBisectCur->set_textContent("Load Current");
	auto applyBisect = [this]()
	{
		client::localStorage.setItem("cxLastBisect", jitBisectArea->get_value());
		client::String* areaStr = jitBisectArea->get_value()->trim();
		if(areaStr->get_length() == 0)
			return;
		auto& lines = *areaStr->split("\n");
		client::Uint32Array* traces = new client::Uint32Array(lines.get_length());
		for (int i = 0; i < lines.get_length(); ++i)
		{
			(*traces)[i] = client::parseInt(lines[i], 16);
		}
		client::Object* result = nullptr;
		__asm__("{type:%1,traces:%2}" : "=r"(result) : "r"(CORE_JIT_BISECT), "r"(traces));
		postMessage(result, /*sendInterrupt*/true);
	};
	if(client::String* o = (client::String*)client::localStorage.getItem("cxLastBisect"))
	{
		jitBisectArea->set_value(o);
		if(o->get_length() != 0)
			applyBisect();
	}
	jitBisectSet->set_onclick(cheerp::Callback(applyBisect));
	jitBisectCur->set_onclick(cheerp::Callback([this]()
	{
		client::Object* result = nullptr;
		__asm__("{type:%1}" : "=r"(result) : "r"(CORE_JIT_GET_CUR_TRACES));
		postMessage(result, /*sendInterrupt*/true);
	}));
	selectContext(nullptr);
	client::document.get_body()->appendChild(hudDiv);
	dbgStartStopBtn->set_onclick(cheerp::Callback([this]
	{
		assert(dbgCurCtx && (dbgCurCtx->state == DBG_DETATCHED || dbgCurCtx->state == DBG_STOPPED));
		client::Object* result = nullptr;
		__asm__("{type:%1,ctxType:%2,value:%3}" : "=r"(result) : "r"(dbgCurCtx->state == DBG_DETATCHED ? CORE_DBG_ATTACH : CORE_DBG_DETACH), "r"(dbgCurCtx->ctxType), "r"(dbgCurCtx->ctxId));
		postMessage(result, /*sendInterrupt*/true);
	}));
	dbgCtxSelect->set_onchange(cheerp::Callback([this]()
	{
		client::String* s = dbgCtxSelect->get_value();
		selectContext(getCtxDataForName(s));
	}));
	updateContexts();
	client::Object* result = nullptr;
	__asm__("{type:%1}" : "=r"(result) : "r"(CORE_ATTACH_HUD));
	postMessage(result, /*sendInterrupt*/true);
	client::setInterval(cheerp::Callback([this]() { updateHud(); }), 1000);
}

void CheerpXBase::updateContexts()
{
	if(dbgCtxSelect == nullptr)
		return;
	while(client::Node* c = dbgCtxSelect->get_firstChild())
		dbgCtxSelect->removeChild(c);
	dbgCtxSelect->appendChild(createOption("<none>", ""));
	for(const ContextData* c: dbgCtxs)
	{
		auto* o = createOption(c->displayName, c->displayName);
		dbgCtxSelect->appendChild(o);
		// Automatically reselect the last selected context
		if(client::localStorage.getItem("cxLastCtx") == c->displayName)
		{
			o->set_selected(true);
			selectContext(c);
		}
	}
}

void CheerpXBase::selectContext(const ContextData* c)
{
	dbgCurCtx = c;
	dbgDisasMode->set_disabled(true);
	dbgDisasAddr->set_disabled(true);
	dbgDisasBtn->set_disabled(true);
	dbgDisasView->set_textContent("");
	if(c == nullptr)
	{
		dbgStartStopBtn->set_textContent("Invalid");
		dbgStartStopBtn->set_disabled(true);
		jitBisectArea->set_disabled(true);
		jitBisectSet->set_disabled(true);
		return;
	}
	else
	{
		jitBisectArea->set_disabled(false);
		jitBisectSet->set_disabled(false);
		client::localStorage.setItem("cxLastCtx", c->displayName);
	}
	switch(c->state)
	{
		case DBG_DETATCHED:
			dbgStartStopBtn->set_textContent("Attach");
			dbgStartStopBtn->set_disabled(false);
			break;
		case DBG_STOPPED:
			dbgStartStopBtn->set_textContent("Detach");
			dbgStartStopBtn->set_disabled(false);
			dbgDisasMode->set_disabled(false);
			dbgDisasAddr->set_disabled(false);
			dbgDisasBtn->set_disabled(false);
			break;
		case DBG_SINGLE_STEP:
			dbgStartStopBtn->set_textContent("Stepping");
			dbgStartStopBtn->set_disabled(true);
			break;
	}
}

client::HTMLOptionElement* CheerpXBase::createOption(const client::String& text, const client::String& value)
{
	client::HTMLOptionElement* o = (client::HTMLOptionElement*)client::document.createElement("option");
	o->set_textContent(text);
	o->set_value(value);
	return o;
}

void CheerpXBase::appendHudBlock(const client::String& blockTitle, client::Element* e)
{
	client::Element* container = client::document.createElement("div");
	client::HTMLElement* p = client::document.createElement("p");
	p->get_style()->set_fontWeight("bold");
	p->set_textContent(blockTitle);
	container->appendChild(p);
	container->appendChild(e);
	hudDiv->appendChild(container);
}
