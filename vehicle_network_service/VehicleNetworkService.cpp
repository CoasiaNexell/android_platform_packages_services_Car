/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#define LOG_TAG "VehicleNetwork"

#include <binder/PermissionCache.h>
#include <utils/Errors.h>
#include <utils/SystemClock.h>

#include <private/android_filesystem_config.h>

#include <vehicle-internal.h>

#include "VehicleHalPropertyUtil.h"
#include "VehicleNetworkService.h"

//#define DBG_MEM_LEAK
//#define DBG_EVENT
//#define DBG_VERBOSE
#ifdef DBG_EVENT
#define EVENT_LOG(x...) ALOGD(x)
#else
#define EVENT_LOG(x...)
#endif
#ifdef DBG_VERBOSE
#define LOG_VERBOSE(x...) ALOGD(x)
#else
#define LOG_VERBOSE(x...)
#endif

namespace android {

#ifdef DBG_MEM_LEAK
// copied from frameworks/base/core/jni/android_os_Debug.cpp

extern "C" void get_malloc_leak_info(uint8_t** info, size_t* overallSize,
    size_t* infoSize, size_t* totalMemory, size_t* backtraceSize);
extern "C" void free_malloc_leak_info(uint8_t* info);
#define SIZE_FLAG_ZYGOTE_CHILD  (1<<31)
#define BACKTRACE_SIZE          32

static size_t gNumBacktraceElements;

/*
 * This is a qsort() callback.
 *
 * See dumpNativeHeap() for comments about the data format and sort order.
 */
static int compareHeapRecords(const void* vrec1, const void* vrec2)
{
    const size_t* rec1 = (const size_t*) vrec1;
    const size_t* rec2 = (const size_t*) vrec2;
    size_t size1 = *rec1;
    size_t size2 = *rec2;

    if (size1 < size2) {
        return 1;
    } else if (size1 > size2) {
        return -1;
    }

    uintptr_t* bt1 = (uintptr_t*)(rec1 + 2);
    uintptr_t* bt2 = (uintptr_t*)(rec2 + 2);
    for (size_t idx = 0; idx < gNumBacktraceElements; idx++) {
        uintptr_t addr1 = bt1[idx];
        uintptr_t addr2 = bt2[idx];
        if (addr1 == addr2) {
            if (addr1 == 0)
                break;
            continue;
        }
        if (addr1 < addr2) {
            return -1;
        } else if (addr1 > addr2) {
            return 1;
        }
    }

    return 0;
}

static void dumpNativeHeap(String8& msg)
{
    uint8_t* info = NULL;
    size_t overallSize, infoSize, totalMemory, backtraceSize;

    get_malloc_leak_info(&info, &overallSize, &infoSize, &totalMemory,
        &backtraceSize);
    if (info == NULL) {
        msg.appendFormat("Native heap dump not available. To enable, run these"
                    " commands (requires root):\n");
        msg.appendFormat("# adb shell stop\n");
        msg.appendFormat("# adb shell setprop libc.debug.malloc.options "
                    "backtrace\n");
        msg.appendFormat("# adb shell start\n");
        return;
    }

    msg.appendFormat("Android Native Heap Dump v1.0\n\n");

    size_t recordCount = overallSize / infoSize;
    msg.appendFormat("Total memory: %zu\n", totalMemory);
    msg.appendFormat("Allocation records: %zd\n", recordCount);
    msg.appendFormat("\n");

    /* re-sort the entries */
    gNumBacktraceElements = backtraceSize;
    qsort(info, recordCount, infoSize, compareHeapRecords);

    /* dump the entries to the file */
    const uint8_t* ptr = info;
    for (size_t idx = 0; idx < recordCount; idx++) {
        size_t size = *(size_t*) ptr;
        size_t allocations = *(size_t*) (ptr + sizeof(size_t));
        uintptr_t* backtrace = (uintptr_t*) (ptr + sizeof(size_t) * 2);

        msg.appendFormat("z %d  sz %8zu  num %4zu  bt",
                (size & SIZE_FLAG_ZYGOTE_CHILD) != 0,
                size & ~SIZE_FLAG_ZYGOTE_CHILD,
                allocations);
        for (size_t bt = 0; bt < backtraceSize; bt++) {
            if (backtrace[bt] == 0) {
                break;
            } else {
#ifdef __LP64__
                msg.appendFormat(" %016" PRIxPTR, backtrace[bt]);
#else
                msg.appendFormat(" %08" PRIxPTR, backtrace[bt]);
#endif
            }
        }
        msg.appendFormat("\n");

        ptr += infoSize;
    }

    free_malloc_leak_info(info);

    msg.appendFormat("MAPS\n");
    const char* maps = "/proc/self/maps";
    FILE* in = fopen(maps, "r");
    if (in == NULL) {
        msg.appendFormat("Could not open %s\n", maps);
        return;
    }
    char buf[BUFSIZ];
    while (size_t n = fread(buf, sizeof(char), BUFSIZ, in)) {
        msg.append(buf, n);
    }
    fclose(in);

    msg.appendFormat("END\n");
}
#endif

// ----------------------------------------------------------------------------

VehicleHalMessageHandler::VehicleHalMessageHandler(const sp<Looper>& looper,
        VehicleNetworkService& service)
    : mLooper(looper),
      mService(service),
      mFreeListIndex(0),
      mLastDispatchTime(0) {
}

VehicleHalMessageHandler::~VehicleHalMessageHandler() {
    Mutex::Autolock autoLock(mLock);
    for (int i = 0; i < NUM_PROPERTY_EVENT_LISTS; i++) {
        for (auto& e : mHalPropertyList[i]) {
            VehiclePropValueUtil::deleteVehiclePropValue(e);
        }
    }
    for (VehicleHalError* e : mHalErrors) {
        delete e;
    }
}

static const int MS_TO_NS = 1000000;

void VehicleHalMessageHandler::handleHalEvent(vehicle_prop_value_t *eventData) {
    EVENT_LOG("handleHalEvent 0x%x", eventData->prop);
    Mutex::Autolock autoLock(mLock);
    List<vehicle_prop_value_t*>& propList = mHalPropertyList[mFreeListIndex];
    propList.push_back(eventData);
    int64_t deltaFromLast = elapsedRealtime() - mLastDispatchTime;
    if (deltaFromLast > DISPATCH_INTERVAL_MS) {
        mLooper->sendMessage(this, Message(HAL_EVENT));
    } else {
        mLooper->sendMessageDelayed((DISPATCH_INTERVAL_MS - deltaFromLast) * MS_TO_NS,
                this, Message(HAL_EVENT));
    }
}

void VehicleHalMessageHandler::handleHalError(VehicleHalError* error) {
    Mutex::Autolock autoLock(mLock);
    mHalErrors.push_back(error);
    mLooper->sendMessage(this, Message(HAL_ERROR));
}

void VehicleHalMessageHandler::handleMockStateChange() {
    Mutex::Autolock autoLock(mLock);
    for (int i = 0; i < 2; i++) {
        for (auto& e : mHalPropertyList[i]) {
            VehiclePropValueUtil::deleteVehiclePropValue(e);
        }
        mHalPropertyList[i].clear();
    }
    sp<MessageHandler> self(this);
    mLooper->removeMessages(self);
}

void VehicleHalMessageHandler::doHandleHalEvent() {
    // event dispatching can take time, so do it outside lock and that requires double buffering.
    // inside lock, free buffer is swapped with non-free buffer.
    List<vehicle_prop_value_t*>* events = NULL;
    do {
        Mutex::Autolock autoLock(mLock);
        mLastDispatchTime = elapsedRealtime();
        int nonFreeListIndex = mFreeListIndex ^ 0x1;
        List<vehicle_prop_value_t*>* nonFreeList = &(mHalPropertyList[nonFreeListIndex]);
        List<vehicle_prop_value_t*>* freeList = &(mHalPropertyList[mFreeListIndex]);
        if (nonFreeList->size() > 0) {
            for (auto& e : *freeList) {
                nonFreeList->push_back(e);
            }
            freeList->clear();
            events = nonFreeList;
        } else if (freeList->size() > 0) {
            events = freeList;
            mFreeListIndex = nonFreeListIndex;
        }
    } while (false);
    if (events != NULL) {
        EVENT_LOG("doHandleHalEvent, num events:%d", events->size());
        mService.dispatchHalEvents(*events);
        //TODO implement return to memory pool
        for (auto& e : *events) {
            VehiclePropValueUtil::deleteVehiclePropValue(e);
        }
        events->clear();
    }
}

void VehicleHalMessageHandler::doHandleHalError() {
    VehicleHalError* error = NULL;
    do {
        Mutex::Autolock autoLock(mLock);
        if (mHalErrors.size() > 0) {
            auto itr = mHalErrors.begin();
            error = *itr;
            mHalErrors.erase(itr);
        }
    } while (false);
    if (error != NULL) {
        mService.dispatchHalError(error);
    }
}

void VehicleHalMessageHandler::handleMessage(const Message& message) {
    switch (message.what) {
    case HAL_EVENT:
        doHandleHalEvent();
        break;
    case HAL_ERROR:
        doHandleHalError();
        break;
    }
}

void VehicleHalMessageHandler::dump(String8& msg) {
    msg.appendFormat("mFreeListIndex:%d, mLastDispatchTime:%" PRId64 "\n",
                     mFreeListIndex, mLastDispatchTime);
}

// ----------------------------------------------------------------------------

void MockDeathHandler::binderDied(const wp<IBinder>& who) {
    mService.handleHalMockDeath(who);
}

// ----------------------------------------------------------------------------

PropertyValueCache::PropertyValueCache() {

}

PropertyValueCache::~PropertyValueCache() {
    for (size_t i = 0; i < mCache.size(); i++) {
        vehicle_prop_value_t* v = mCache.editValueAt(i);
        VehiclePropValueUtil::deleteVehiclePropValue(v);
    }
    mCache.clear();
}

void PropertyValueCache::writeToCache(const vehicle_prop_value_t& value) {
    vehicle_prop_value_t* v;
    ssize_t index = mCache.indexOfKey(value.prop);
    if (index < 0) {
        v = VehiclePropValueUtil::allocVehiclePropValue(value);
        ASSERT_OR_HANDLE_NO_MEMORY(v, return);
        mCache.add(value.prop, v);
    } else {
        v = mCache.editValueAt(index);
        VehiclePropValueUtil::copyVehiclePropValue(v, value, true /* deleteOldData */);
    }
}

bool PropertyValueCache::readFromCache(vehicle_prop_value_t* value) {
    ssize_t index = mCache.indexOfKey(value->prop);
    if (index < 0) {
        ALOGE("readFromCache 0x%x, not found", value->prop);
        return false;
    }
    const vehicle_prop_value_t* cached = mCache.valueAt(index);
    status_t r = VehiclePropValueUtil::copyVehiclePropValue(value, *cached);
    if (r != NO_ERROR) {
        ALOGD("readFromCache 0x%x, copy failed %d", value->prop, r);
        return false;
    }
    return true;
}

// ----------------------------------------------------------------------------


VehicleNetworkService* VehicleNetworkService::sInstance = NULL;

status_t VehicleNetworkService::dump(int fd, const Vector<String16>& /*args*/) {
    static const String16 sDump("android.permission.DUMP");
    String8 msg;
    if (!PermissionCache::checkCallingPermission(sDump)) {
        msg.appendFormat("Permission Denial: "
                    "can't dump VNS from pid=%d, uid=%d\n",
                    IPCThreadState::self()->getCallingPid(),
                    IPCThreadState::self()->getCallingUid());
        write(fd, msg.string(), msg.size());
        return NO_ERROR;
    }
#ifdef DBG_MEM_LEAK
    dumpNativeHeap(msg);
#endif
    msg.appendFormat("MockingEnabled=%d\n", mMockingEnabled ? 1 : 0);
    msg.appendFormat("*Handler, now in ms:%" PRId64 "\n", elapsedRealtime());
    mHandler->dump(msg);
    msg.append("*Properties\n");
    for (auto& prop : mProperties->getList()) {
        VechilePropertyUtil::dumpProperty(msg, *prop);
    }
    if (mMockingEnabled) {
        msg.append("*Mocked Properties\n");
        for (auto& prop : mPropertiesForMocking->getList()) {
            msg.appendFormat("property 0x%x\n", prop->prop);
        }
    }
    msg.append("*Active clients*\n");
    for (size_t i = 0; i < mBinderToClientMap.size(); i++) {
        mBinderToClientMap.valueAt(i)->dump(msg);
    }
    msg.append("*Active clients per property*\n");
    for (size_t i = 0; i < mPropertyToClientsMap.size(); i++) {
        msg.appendFormat("prop 0x%x, pids:", mPropertyToClientsMap.keyAt(i));
        sp<HalClientSpVector> clients = mPropertyToClientsMap.valueAt(i);
        for (size_t j = 0; j < clients->size(); j++) {
            msg.appendFormat("%d,", clients->itemAt(j)->getPid());
        }
        msg.append("\n");
    }
    msg.append("*Subscription info per property*\n");
    for (size_t i = 0; i < mSubscriptionInfos.size(); i++) {
        const SubscriptionInfo& info = mSubscriptionInfos.valueAt(i);
        msg.appendFormat("prop 0x%x, sample rate %f Hz, zones 0x%x, flags: 0x%x\n",
                         mSubscriptionInfos.keyAt(i), info.sampleRate, info.zones, info.flags);
    }
    msg.appendFormat("*Event info per property, now in ns:%" PRId64 " *\n", elapsedRealtimeNano());
    for (size_t i = 0; i < mEventInfos.size(); i++) {
        const EventInfo& info = mEventInfos.valueAt(i);
        msg.appendFormat("prop 0x%x, event counts:%d, last timestamp: %" PRId64 "\n",
                mEventInfos.keyAt(i), info.eventCount, info.lastTimestamp);
    }
    msg.appendFormat(" Events dropped while in mocking:%d, last dropped time %" PRId64 "\n",
                     mDroppedEventsWhileInMocking, mLastEventDropTimeWhileInMocking);
    msg.append("*Vehicle Network Service Permissions*\n");
    mVehiclePropertyAccessControl.dump(msg);
    msg.append("*Vehicle HAL dump*\n");
    write(fd, msg.string(), msg.size());
    mDevice->dump(mDevice, fd);
    return NO_ERROR;
}

bool VehicleNetworkService::isOperationAllowed(int32_t property, bool isWrite) {
    const uid_t uid = IPCThreadState::self()->getCallingUid();

    bool allowed = mVehiclePropertyAccessControl.testAccess(property, uid, isWrite);
    if (!allowed) {
        ALOGW("Property 0x%x: access not allowed for uid %d, isWrite %d", property, uid, isWrite);
    }
    return allowed;
}

VehicleNetworkService::VehicleNetworkService()
    : mModule(NULL),
      mPropertiesSubscribedToSetCall(0),
      mMockingEnabled(false),
      mDroppedEventsWhileInMocking(0),
      mLastEventDropTimeWhileInMocking(0){
    sInstance = this;

   // Load vehicle network services policy file
   if(!mVehiclePropertyAccessControl.init()) {
     LOG_ALWAYS_FATAL("Vehicle property access policy could not be initialized.");
   }
}

VehicleNetworkService::~VehicleNetworkService() {
    sInstance = NULL;
    for (size_t i = 0; i < mPropertyToClientsMap.size(); i++) {
        sp<HalClientSpVector> clients = mPropertyToClientsMap.editValueAt(i);
        clients->clear();
    }
    mBinderToClientMap.clear();
    mPropertyToClientsMap.clear();
    mSubscriptionInfos.clear();
}

void VehicleNetworkService::binderDied(const wp<IBinder>& who) {
    List<int32_t> propertiesToUnsubscribe;
    do {
        Mutex::Autolock autoLock(mLock);
        sp<IBinder> ibinder = who.promote();
        ibinder->unlinkToDeath(this);
        ssize_t index = mBinderToClientMap.indexOfKey(ibinder);
        if (index < 0) {
            // already removed. ignore
            return;
        }
        sp<HalClient> currentClient = mBinderToClientMap.editValueAt(index);
        ALOGW("client binder death, pid: %d, uid:%d", currentClient->getPid(),
                currentClient->getUid());
        mBinderToClientMap.removeItemsAt(index);

        for (size_t i = 0; i < mPropertyToClientsMap.size(); i++) {
            sp<HalClientSpVector>& clients = mPropertyToClientsMap.editValueAt(i);
            clients->remove(currentClient);
            if (clients->size() == 0) {
                int32_t property = mPropertyToClientsMap.keyAt(i);
                propertiesToUnsubscribe.push_back(property);
                mSubscriptionInfos.removeItem(property);
            }
        }
        for (int32_t property : propertiesToUnsubscribe) {
            mPropertyToClientsMap.removeItem(property);
        }
    } while (false);
    for (int32_t property : propertiesToUnsubscribe) {
        mDevice->unsubscribe(mDevice, property);
    }
}

void VehicleNetworkService::handleHalMockDeath(const wp<IBinder>& who) {
    ALOGE("Hal mock binder died");
    sp<IBinder> ibinder = who.promote();
    stopMocking(IVehicleNetworkHalMock::asInterface(ibinder));
}

int VehicleNetworkService::eventCallback(const vehicle_prop_value_t *eventData) {
    EVENT_LOG("eventCallback 0x%x");
    sInstance->onHalEvent(eventData);
    return NO_ERROR;
}

int VehicleNetworkService::errorCallback(int32_t errorCode, int32_t property, int32_t operation) {
    status_t r = sInstance->onHalError(errorCode, property, operation);
    if (r != NO_ERROR) {
        ALOGE("VehicleNetworkService::errorCallback onHalError failed with %d", r);
    }
    return NO_ERROR;
}

extern "C" {
vehicle_prop_config_t const * getInternalProperties();
int getNumInternalProperties();
};

void VehicleNetworkService::onFirstRef() {
    Mutex::Autolock autoLock(mLock);
    status_t r = loadHal();
    if (r!= NO_ERROR) {
        ALOGE("cannot load HAL, error:%d", r);
        return;
    }
    mHandlerThread = new HandlerThread();
    r = mHandlerThread->start("HAL.NATIVE_LOOP");
    if (r != NO_ERROR) {
        ALOGE("cannot start handler thread, error:%d", r);
        return;
    }
    sp<VehicleHalMessageHandler> handler(new VehicleHalMessageHandler(mHandlerThread->getLooper(),
            *this));
    ASSERT_ALWAYS_ON_NO_MEMORY(handler.get());
    mHandler = handler;

    // populate empty list before hal init.
    mProperties = new VehiclePropertiesHolder(false /* deleteConfigsInDestructor */);
    ASSERT_ALWAYS_ON_NO_MEMORY(mProperties);

    r = mDevice->init(mDevice, eventCallback, errorCallback);
    if (r != NO_ERROR) {
        ALOGE("HAL init failed:%d", r);
        return;
    }
    int numConfigs = 0;
    vehicle_prop_config_t const* configs = mDevice->list_properties(mDevice, &numConfigs);
    for (int i = 0; i < numConfigs; i++) {
        mProperties->getList().push_back(&configs[i]);
    }
    configs = getInternalProperties();
    for (int i = 0; i < getNumInternalProperties(); i++) {
        mProperties->getList().push_back(&configs[i]);
    }
}

void VehicleNetworkService::release() {
    do {
        Mutex::Autolock autoLock(mLock);
        mHandlerThread->quit();
    } while (false);
    mDevice->release(mDevice);
}

vehicle_prop_config_t const * VehicleNetworkService::findConfigLocked(int32_t property) {
    // TODO: this method is called on every get/set, consider to use hash map.
    for (auto& config : (mMockingEnabled ?
            mPropertiesForMocking->getList() : mProperties->getList())) {
        if (config->prop == property) {
            return config;
        }
    }
    ALOGW("property not found 0x%x", property);
    return NULL;
}

bool VehicleNetworkService::isGettableLocked(int32_t property) {
    vehicle_prop_config_t const * config = findConfigLocked(property);
    if (config == NULL) {
        return false;
    }
    if ((config->access & VEHICLE_PROP_ACCESS_READ) == 0) {
        ALOGI("cannot get, property 0x%x is write only", property);
        return false;
    }
    return true;
}

bool VehicleNetworkService::isSettableLocked(int32_t property, int32_t valueType) {
    vehicle_prop_config_t const * config = findConfigLocked(property);
    if (config == NULL) {
        return false;
    }
    if ((config->access & VEHICLE_PROP_ACCESS_WRITE) == 0) {
        ALOGI("cannot set, property 0x%x is read only", property);
        return false;
    }
    if (config->value_type != valueType) {
        ALOGW("cannot set, property 0x%x expects type 0x%x while got 0x%x", property,
                config->value_type, valueType);
        return false;
    }
    return true;
}

bool VehicleNetworkService::isSubscribableLocked(int32_t property) {
    vehicle_prop_config_t const * config = findConfigLocked(property);
    if (config == NULL) {
        return false;
    }
    if ((config->access & VEHICLE_PROP_ACCESS_READ) == 0) {
        ALOGI("cannot subscribe, property 0x%x is write only", property);
        return false;
    }
    if (config->change_mode == VEHICLE_PROP_CHANGE_MODE_STATIC) {
        ALOGI("cannot subscribe, property 0x%x is static", property);
        return false;
    }
    if (config->change_mode == VEHICLE_PROP_CHANGE_MODE_POLL) {
            ALOGI("cannot subscribe, property 0x%x is poll only", property);
            return false;
    }
    return true;
}

bool VehicleNetworkService::isZonedProperty(vehicle_prop_config_t const * config) {
    if (config == NULL) {
        return false;
    }
    switch (config->value_type) {
    case VEHICLE_VALUE_TYPE_ZONED_INT32:
    case VEHICLE_VALUE_TYPE_ZONED_FLOAT:
    case VEHICLE_VALUE_TYPE_ZONED_BOOLEAN:
    case VEHICLE_VALUE_TYPE_ZONED_INT32_VEC2:
    case VEHICLE_VALUE_TYPE_ZONED_INT32_VEC3:
    case VEHICLE_VALUE_TYPE_ZONED_FLOAT_VEC2:
    case VEHICLE_VALUE_TYPE_ZONED_FLOAT_VEC3:
        return true;
    }
    return false;
}

sp<VehiclePropertiesHolder> VehicleNetworkService::listProperties(int32_t property) {
    Mutex::Autolock autoLock(mLock);
    if (property == 0) {
        if (!mMockingEnabled) {
            return mProperties;
        } else {
            return mPropertiesForMocking;
        }
    } else {
        sp<VehiclePropertiesHolder> p;
        const vehicle_prop_config_t* config = findConfigLocked(property);
        if (config != NULL) {
            p = new VehiclePropertiesHolder(false /* deleteConfigsInDestructor */);
            p->getList().push_back(config);
            ASSERT_OR_HANDLE_NO_MEMORY(p.get(), return p);
        }
        return p;
    }
}

status_t VehicleNetworkService::getProperty(vehicle_prop_value_t *data) {
    bool inMocking = false;
    do { // for lock scoping
        Mutex::Autolock autoLock(mLock);
        if (!isGettableLocked(data->prop)) {
            ALOGW("getProperty, cannot get 0x%x", data->prop);
            return BAD_VALUE;
        }
        if ((data->prop >= (int32_t)VEHICLE_PROPERTY_INTERNAL_START) &&
                (data->prop <= (int32_t)VEHICLE_PROPERTY_INTERNAL_END)) {
            if (!mCache.readFromCache(data)) {
                return BAD_VALUE;
            }
            return NO_ERROR;
        }
        //TODO caching for static, on-change type?
        if (mMockingEnabled) {
            inMocking = true;
        }
    } while (false);
    // set done outside lock to allow concurrent access
    if (inMocking) {
        status_t r = mHalMock->onPropertyGet(data);
        if (r != NO_ERROR) {
            ALOGW("getProperty 0x%x failed, mock returned %d", data->prop, r);
        }
        return r;
    }
    /*
     * get call can return -EAGAIN error when hal has not fetched all data. In that case,
     * keep retrying for certain time with some sleep. This will happen only at initial stage.
     */
    status_t r = -EAGAIN;
    int retryCount = 0;
    while (true) {
        r = mDevice->get(mDevice, data);
        if (r == NO_ERROR) {
            break;
        }
        if (r != -EAGAIN) {
            break;
        }
        retryCount++;
        if (retryCount > MAX_GET_SET_RETRY_NUMBER_FOR_NOT_READY) {
            ALOGE("Vehicle hal get, not ready after multiple retries");
            break;
        }
        usleep(GET_SET_WAIT_TIME_US);
    }
    if (r != NO_ERROR) {
        ALOGW("getProperty 0x%x failed, HAL returned %d", data->prop, r);
    }
    return r;
}

void VehicleNetworkService::releaseMemoryFromGet(vehicle_prop_value_t* value) {
    switch (value->value_type) {
    case VEHICLE_VALUE_TYPE_STRING:
    case VEHICLE_VALUE_TYPE_BYTES: {
        Mutex::Autolock autoLock(mLock);
        if (mMockingEnabled) {
            VehiclePropValueUtil::deleteMembers(value);
        } else {
            mDevice->release_memory_from_get(mDevice, value);
        }
    } break;
    }
}

status_t VehicleNetworkService::setProperty(const vehicle_prop_value_t& data) {
    bool inMocking = false;
    bool isInternalProperty = data.prop >= VEHICLE_PROPERTY_INTERNAL_START
                              && data.prop <= VEHICLE_PROPERTY_INTERNAL_END;
    sp<HalClientSpVector> propertyClientsForSetEvent;
    do { // for lock scoping
        Mutex::Autolock autoLock(mLock);
        if (!isSettableLocked(data.prop, data.value_type)) {
            ALOGW("setProperty, cannot set 0x%x", data.prop);
            return BAD_VALUE;
        }
        if (isInternalProperty) {
            mCache.writeToCache(data);
        }
        if (mMockingEnabled) {
            inMocking = true;
        }
        if (mPropertiesSubscribedToSetCall.count(data.prop) == 1) {
            propertyClientsForSetEvent = findClientsVectorForPropertyLocked(data.prop);
        }
    } while (false);
    if (inMocking) {
        status_t r = mHalMock->onPropertySet(data);
        if (r != NO_ERROR) {
            ALOGW("setProperty 0x%x failed, mock returned %d", data.prop, r);
            return r;
        }
    }

    if (propertyClientsForSetEvent.get() != NULL && propertyClientsForSetEvent->size() > 0) {
        dispatchPropertySetEvent(data, propertyClientsForSetEvent);
    }

    if (isInternalProperty) {
        // for internal property, just publish it.
        onHalEvent(&data, inMocking);
        return NO_ERROR;
    }
    if (inMocking) {
        return NO_ERROR;
    }
    // set done outside lock to allow concurrent access
    /*
     * set call can return -EAGAIN error when hal has not fetched all data. In that case,
     * keep retrying for certain time with some sleep. This will happen only at initial stage.
     */
    status_t r = -EAGAIN;
    int retryCount = 0;
    while (true) {
        r = mDevice->set(mDevice, &data);
        if (r == NO_ERROR) {
            break;
        }
        if (r != -EAGAIN) {
            break;
        }
        retryCount++;
        if (retryCount > MAX_GET_SET_RETRY_NUMBER_FOR_NOT_READY) {
            ALOGE("Vehicle hal set, not ready after multiple retries");
            break;
        }
        usleep(GET_SET_WAIT_TIME_US);
    }
    if (r != NO_ERROR) {
        ALOGW("setProperty 0x%x failed, HAL returned %d", data.prop, r);
    }
    return r;
}

void VehicleNetworkService::dispatchPropertySetEvent(
        const vehicle_prop_value_t& data, const sp<HalClientSpVector>& clientsForProperty) {
    for (size_t i = 0; i < clientsForProperty->size(); i++) {
        sp<HalClient> client = clientsForProperty->itemAt(i);
        SubscriptionInfo* subscription = client->getSubscriptionInfo(data.prop);

        bool shouldDispatch =
                subscription != NULL
                && (SubscribeFlags::SET_CALL & subscription->flags)
                && (data.zone == subscription->zones || (data.zone & subscription->zones));

        if (shouldDispatch) {
            client->dispatchPropertySetEvent(data);
        }
    }
}

bool isSampleRateFixed(int32_t changeMode) {
    switch (changeMode) {
    case VEHICLE_PROP_CHANGE_MODE_ON_CHANGE:
    case VEHICLE_PROP_CHANGE_MODE_ON_SET:
        return true;
    }
    return false;
}

status_t VehicleNetworkService::subscribe(const sp<IVehicleNetworkListener> &listener, int32_t prop,
        float sampleRate, int32_t zones, int32_t flags) {
    bool shouldSubscribe = false;
    bool inMock = false;
    int32_t newZones = zones;
    vehicle_prop_config_t const * config = NULL;
    sp<HalClient> client;
    bool autoGetEnabled = false;

    if (flags == SubscribeFlags::UNDEFINED) {
        flags = SubscribeFlags::DEFAULT;
    }

    do {
        Mutex::Autolock autoLock(mLock);
        if (!isSubscribableLocked(prop)) {
            return BAD_VALUE;
        }
        config = findConfigLocked(prop);
        if ((flags & SubscribeFlags::SET_CALL)
            && !(config->access & VEHICLE_PROP_ACCESS_WRITE)) {
            ALOGE("Attempt to subscribe with SUBSCRIBE_TO_SET flag to prop: 0x%x that doesn't have"
                          " write access", prop);
            return BAD_VALUE;
        }

        if (isSampleRateFixed(config->change_mode)) {
            if (sampleRate != 0) {
                ALOGW("Sample rate set to non-zeo for on change type. Ignore it");
                sampleRate = 0;
            }
        } else {
            if (sampleRate > config->max_sample_rate) {
                ALOGW("sample rate %f higher than max %f. limit to max", sampleRate,
                        config->max_sample_rate);
                sampleRate = config->max_sample_rate;
            }
            if (sampleRate < config->min_sample_rate) {
                ALOGW("sample rate %f lower than min %f. limit to min", sampleRate,
                                    config->min_sample_rate);
                sampleRate = config->min_sample_rate;
            }
        }
        if (isZonedProperty(config)) {
            if ((zones != 0) && ((zones & config->vehicle_zone_flags) != zones)) {
                ALOGE("subscribe requested zones 0x%x out of range, supported:0x%x", zones,
                        config->vehicle_zone_flags);
                return BAD_VALUE;
            }
        } else { // ignore zone
            zones = 0;
        }
        sp<IBinder> ibinder = IInterface::asBinder(listener);
        LOG_VERBOSE("subscribe, binder 0x%x prop 0x%x", ibinder.get(), prop);
        client = findOrCreateClientLocked(ibinder, listener);
        if (client.get() == NULL) {
            ALOGE("subscribe, no memory, cannot create HalClient");
            return NO_MEMORY;
        }
        sp<HalClientSpVector> clientsForProperty = findOrCreateClientsVectorForPropertyLocked(prop);
        if (clientsForProperty.get() == NULL) {
            ALOGE("subscribe, no memory, cannot create HalClientSpVector");
            return NO_MEMORY;
        }
        clientsForProperty->add(client);
        ssize_t index = mSubscriptionInfos.indexOfKey(prop);
        if (index < 0) {
            // first time subscription for this property
            shouldSubscribe = true;
        } else {
            const SubscriptionInfo& info = mSubscriptionInfos.valueAt(index);
            if (info.sampleRate < sampleRate) {
                shouldSubscribe = true;
            }
            newZones = (info.zones == 0) ? 0 : ((zones == 0) ? 0 : (info.zones | zones));
            if (info.zones != newZones) {
                shouldSubscribe = true;
            }
            if (info.flags != flags) {  // Flags has been changed, need to update subscription.
                shouldSubscribe = true;
            }
        }
        if (SubscribeFlags::SET_CALL & flags) {
            mPropertiesSubscribedToSetCall.insert(prop);
        }
        client->setSubscriptionInfo(prop, sampleRate, zones, flags);
        if (shouldSubscribe) {
            autoGetEnabled = mVehiclePropertyAccessControl.isAutoGetEnabled(prop);
            inMock = mMockingEnabled;
            SubscriptionInfo info(sampleRate, newZones, flags);
            mSubscriptionInfos.add(prop, info);
            if ((prop >= (int32_t)VEHICLE_PROPERTY_INTERNAL_START) &&
                                (prop <= (int32_t)VEHICLE_PROPERTY_INTERNAL_END)) {
                LOG_VERBOSE("subscribe to internal property, prop 0x%x", prop);
                return NO_ERROR;
            }
        }
    } while (false);
    if (shouldSubscribe && (SubscribeFlags::HAL_EVENT & flags)) {
        if (inMock) {
            status_t r = mHalMock->onPropertySubscribe(prop, sampleRate, newZones);
            if (r != NO_ERROR) {
                ALOGW("subscribe 0x%x failed, mock returned %d", prop, r);
                return r;
            }
        } else {
            LOG_VERBOSE("subscribe to HAL, prop 0x%x sample rate:%f zones:0x%x", prop, sampleRate,
                    newZones);
            status_t r = mDevice->subscribe(mDevice, prop, sampleRate, newZones);
            if (r != NO_ERROR) {
                ALOGW("subscribe 0x%x failed, HAL returned %d", prop, r);
                return r;
            }
        }
    }
    if (autoGetEnabled && isSampleRateFixed(config->change_mode)) {
        status_t r = notifyClientWithCurrentValue(inMock, config, zones);
        if (r != NO_ERROR) {
            return r;
        }
    }
    return NO_ERROR;
}

status_t VehicleNetworkService::notifyClientWithCurrentValue(bool isMocking,
        const vehicle_prop_config_t *config, int32_t zones) {
    status_t r;
    int32_t prop = config->prop;
    int32_t valueType = config->value_type;
    if (isZonedProperty(config)) {
        int32_t requestedZones = (zones == 0) ? config->vehicle_zone_flags : zones;
        for (int i = 0, zone = 1; i < 32; i++, zone <<= 1) {
            if ((zone & requestedZones) == zone) {
                    r = notifyClientWithCurrentValue(isMocking, prop, valueType, zone);
                    if (r != NO_ERROR) {
                        return r;
                    }
                }
            }
        } else {
            r = notifyClientWithCurrentValue(isMocking, prop, valueType, 0 /*no zones*/);
            if (r != NO_ERROR) {
                return r;
            }
        }
    return NO_ERROR;
}

status_t VehicleNetworkService::notifyClientWithCurrentValue(bool isMocking,
    int32_t prop, int32_t valueType, int32_t zone) {
    std::unique_ptr<vehicle_prop_value_t> v(new vehicle_prop_value_t());
    ASSERT_OR_HANDLE_NO_MEMORY(v.get(), return NO_MEMORY);
    memset(v.get(), 0, sizeof(vehicle_prop_value_t));
    v->prop = prop;
    v->value_type = valueType;
    v->zone = zone;
    status_t r = isMocking ? mHalMock->onPropertyGet(v.get()) : mDevice->get(mDevice, v.get());
    if (r != NO_ERROR) {
        if (r == -EAGAIN) {
            LOG_VERBOSE("value is not ready:0x%x, mock:%d", prop, isMocking);
            return NO_ERROR;
        } else {
            ALOGW("failed to get current value prop:0x%x, mock:%d, error:%d", prop, isMocking, r);
            return r;
        }
    }
    if (isMocking) {
        // no copy and vehicle_prop_value_t is passed to event handler
        onHalEvent(v.release(), false, false /*do not copy */);
    } else {
        // copy is made, and vehicle_prop_value_t deleted in this function
        onHalEvent(v.get(), false, true /*do copy */);
        releaseMemoryFromGet(v.get());
    }

    return NO_ERROR;
}

void VehicleNetworkService::unsubscribe(const sp<IVehicleNetworkListener> &listener, int32_t prop) {
    bool shouldUnsubscribe = false;
    bool inMocking = false;
    do {
        Mutex::Autolock autoLock(mLock);
        if (!isSubscribableLocked(prop)) {
            return;
        }
        sp<IBinder> ibinder = IInterface::asBinder(listener);
        LOG_VERBOSE("unsubscribe, binder 0x%x, prop 0x%x", ibinder.get(), prop);
        sp<HalClient> client = findClientLocked(ibinder);
        if (client.get() == NULL) {
            ALOGD("unsubscribe client not found in binder map");
            return;
        }
        shouldUnsubscribe = removePropertyFromClientLocked(ibinder, client, prop);
        if ((prop >= (int32_t)VEHICLE_PROPERTY_INTERNAL_START) &&
                (prop <= (int32_t)VEHICLE_PROPERTY_INTERNAL_END)) {
            LOG_VERBOSE("unsubscribe to internal property, prop 0x%x", prop);
            return;
        }
        if (mMockingEnabled) {
            inMocking = true;
        }
    } while (false);
    if (shouldUnsubscribe) {
        if (inMocking) {
            mHalMock->onPropertyUnsubscribe(prop);
        } else {
            mDevice->unsubscribe(mDevice, prop);
        }
    }
}

sp<HalClient> VehicleNetworkService::findClientLocked(sp<IBinder>& ibinder) {
    sp<HalClient> client;
    ssize_t index = mBinderToClientMap.indexOfKey(ibinder);
    if (index < 0) {
        return client;
    }
    return mBinderToClientMap.editValueAt(index);
}

sp<HalClient> VehicleNetworkService::findOrCreateClientLocked(sp<IBinder>& ibinder,
        const sp<IVehicleNetworkListener> &listener) {
    sp<HalClient> client;
    ssize_t index = mBinderToClientMap.indexOfKey(ibinder);
    if (index < 0) {
        IPCThreadState* self = IPCThreadState::self();
        pid_t pid = self->getCallingPid();
        uid_t uid = self->getCallingUid();
        client = new HalClient(listener, pid, uid);
        ASSERT_OR_HANDLE_NO_MEMORY(client.get(), return client);
        ibinder->linkToDeath(this);
        LOG_VERBOSE("add binder 0x%x to map", ibinder.get());
        mBinderToClientMap.add(ibinder, client);
    } else {
        client = mBinderToClientMap.editValueAt(index);
    }
    return client;
}

sp<HalClientSpVector> VehicleNetworkService::findClientsVectorForPropertyLocked(int32_t property) {
    sp<HalClientSpVector> clientsForProperty;
    ssize_t index = mPropertyToClientsMap.indexOfKey(property);
    if (index >= 0) {
        clientsForProperty = mPropertyToClientsMap.editValueAt(index);
    }
    return clientsForProperty;
}

sp<HalClientSpVector> VehicleNetworkService::findOrCreateClientsVectorForPropertyLocked(
        int32_t property) {
    sp<HalClientSpVector> clientsForProperty;
    ssize_t index = mPropertyToClientsMap.indexOfKey(property);
    if (index >= 0) {
        clientsForProperty = mPropertyToClientsMap.editValueAt(index);
    } else {
        clientsForProperty = new HalClientSpVector();
        ASSERT_OR_HANDLE_NO_MEMORY(clientsForProperty.get(), return clientsForProperty);
        mPropertyToClientsMap.add(property, clientsForProperty);
    }
    return clientsForProperty;
}

bool VehicleNetworkService::hasClientsSubscribedToSetCallLocked(
        int32_t property, const sp<HalClientSpVector> &clientsForProperty) const {
    for (size_t i = 0; i < clientsForProperty->size(); i++) {
        sp<HalClient> c = clientsForProperty->itemAt(i);
        SubscriptionInfo* subscription = c->getSubscriptionInfo(property);
        if (subscription != NULL && (SubscribeFlags::SET_CALL & subscription->flags)) {
            return true;
        }
    }
    return false;
}

/**
 * remove given property from client and remove HalClient if necessary.
 * @return true if the property should be unsubscribed from HAL (=no more clients).
 */
bool VehicleNetworkService::removePropertyFromClientLocked(sp<IBinder>& ibinder,
        sp<HalClient>& client, int32_t property) {
    if(!client->removePropertyAndCheckIfActive(property)) {
        // client is no longer necessary
        mBinderToClientMap.removeItem(ibinder);
        ibinder->unlinkToDeath(this);
    }
    sp<HalClientSpVector> clientsForProperty = findClientsVectorForPropertyLocked(property);
    if (clientsForProperty.get() == NULL) {
        // no subscription
        return false;
    }
    clientsForProperty->remove(client);

    if (!hasClientsSubscribedToSetCallLocked(property, clientsForProperty)) {
        mPropertiesSubscribedToSetCall.erase(property);
    }

    //TODO reset sample rate. do not care for now.
    if (clientsForProperty->size() == 0) {
        mPropertyToClientsMap.removeItem(property);
        mSubscriptionInfos.removeItem(property);
        return true;
    }
    return false;
}

status_t VehicleNetworkService::injectEvent(const vehicle_prop_value_t& value) {
    ALOGI("injectEvent property:0x%x", value.prop);
    return onHalEvent(&value, true);
}

status_t VehicleNetworkService::startMocking(const sp<IVehicleNetworkHalMock>& mock) {
    sp<VehicleHalMessageHandler> handler;
    List<sp<HalClient> > clientsToDispatch;
    do {
        Mutex::Autolock autoLock(mLock);
        if (mMockingEnabled) {
            ALOGW("startMocking while already enabled");
            // allow it as test can fail without clearing
            if (mHalMock != NULL) {
                IInterface::asBinder(mHalMock)->unlinkToDeath(mHalMockDeathHandler.get());
            }
        }
        ALOGW("starting vehicle HAL mocking");
        sp<IBinder> ibinder = IInterface::asBinder(mock);
        mHalMockDeathHandler = new MockDeathHandler(*this);
        ibinder->linkToDeath(mHalMockDeathHandler);
        mHalMock = mock;
        mMockingEnabled = true;
        // Mock implementation should make sure that its startMocking call is not blocking its
        // onlistProperties call. Otherwise, this will lead into dead-lock.
        mPropertiesForMocking = mock->onListProperties();
        handleHalRestartAndGetClientsToDispatchLocked(clientsToDispatch);
        handler = mHandler;
    } while (false);
    handler->handleMockStateChange();
    for (auto& client : clientsToDispatch) {
        client->dispatchHalRestart(true);
    }
    clientsToDispatch.clear();
    return NO_ERROR;
}

void VehicleNetworkService::stopMocking(const sp<IVehicleNetworkHalMock>& mock) {
    List<sp<HalClient> > clientsToDispatch;
    sp<VehicleHalMessageHandler> handler;
    do {
        Mutex::Autolock autoLock(mLock);
        if (mHalMock.get() == NULL) {
            return;
        }
        sp<IBinder> ibinder = IInterface::asBinder(mock);
        if (ibinder != IInterface::asBinder(mHalMock)) {
            ALOGE("stopMocking, not the one started");
            return;
        }
        ALOGW("stopping vehicle HAL mocking");
        ibinder->unlinkToDeath(mHalMockDeathHandler.get());
        mHalMockDeathHandler.clear();
        mHalMock.clear();
        mMockingEnabled = false;
        mPropertiesForMocking.clear();
        handleHalRestartAndGetClientsToDispatchLocked(clientsToDispatch);
        handler = mHandler;
    } while (false);
    handler->handleMockStateChange();
    for (auto& client : clientsToDispatch) {
        client->dispatchHalRestart(false);
    }
    clientsToDispatch.clear();
}

void VehicleNetworkService::handleHalRestartAndGetClientsToDispatchLocked(
        List<sp<HalClient> >& clientsToDispatch) {
    // all subscriptions are invalid
    mPropertyToClientsMap.clear();
    mSubscriptionInfos.clear();
    mEventInfos.clear();
    List<sp<HalClient> > clientsToRemove;
    for (size_t i = 0; i < mBinderToClientMap.size(); i++) {
        sp<HalClient> client = mBinderToClientMap.valueAt(i);
        client->removeAllProperties();
        if (client->isMonitoringHalRestart()) {
            clientsToDispatch.push_back(client);
        }
        if (!client->isActive()) {
            clientsToRemove.push_back(client);
        }
    }
    for (auto& client : clientsToRemove) {
        // client is no longer necessary
        sp<IBinder> ibinder = IInterface::asBinder(client->getListener());
        mBinderToClientMap.removeItem(ibinder);
        ibinder->unlinkToDeath(this);
    }
    clientsToRemove.clear();
}

status_t VehicleNetworkService::injectHalError(int32_t errorCode, int32_t property,
        int32_t operation) {
    return onHalError(errorCode, property, operation, true /*isInjection*/);
}

status_t VehicleNetworkService::startErrorListening(const sp<IVehicleNetworkListener> &listener) {
    sp<IBinder> ibinder = IInterface::asBinder(listener);
    sp<HalClient> client;
    do {
        Mutex::Autolock autoLock(mLock);
        client = findOrCreateClientLocked(ibinder, listener);
    } while (false);
    if (client.get() == NULL) {
        ALOGW("startErrorListening failed, no memory");
        return NO_MEMORY;
    }
    client->setHalErrorMonitoringState(true);
    return NO_ERROR;
}

void VehicleNetworkService::stopErrorListening(const sp<IVehicleNetworkListener> &listener) {
    sp<IBinder> ibinder = IInterface::asBinder(listener);
    sp<HalClient> client;
    do {
        Mutex::Autolock autoLock(mLock);
        client = findClientLocked(ibinder);
    } while (false);
    if (client.get() != NULL) {
        client->setHalErrorMonitoringState(false);
    }
}

status_t VehicleNetworkService::startHalRestartMonitoring(
        const sp<IVehicleNetworkListener> &listener) {
    sp<IBinder> ibinder = IInterface::asBinder(listener);
    sp<HalClient> client;
    do {
        Mutex::Autolock autoLock(mLock);
        client = findOrCreateClientLocked(ibinder, listener);
    } while (false);
    if (client.get() == NULL) {
        ALOGW("startHalRestartMonitoring failed, no memory");
        return NO_MEMORY;
    }
    client->setHalRestartMonitoringState(true);
    return NO_ERROR;
}

void VehicleNetworkService::stopHalRestartMonitoring(const sp<IVehicleNetworkListener> &listener) {
    sp<IBinder> ibinder = IInterface::asBinder(listener);
    sp<HalClient> client;
    do {
        Mutex::Autolock autoLock(mLock);
        client = findClientLocked(ibinder);
    } while (false);
    if (client.get() != NULL) {
        client->setHalRestartMonitoringState(false);
    }
}

status_t VehicleNetworkService::onHalEvent(const vehicle_prop_value_t* eventData, bool isInjection,
        bool doCopy)
{
    sp<VehicleHalMessageHandler> handler;
    do {
        Mutex::Autolock autoLock(mLock);
        if (!isInjection && mMockingEnabled) {
            // drop real HAL event if mocking is enabled
            mDroppedEventsWhileInMocking++;
            mLastEventDropTimeWhileInMocking = elapsedRealtimeNano();
            if (!doCopy) { // ownership passed here. so should delete here.
                VehiclePropValueUtil::deleteVehiclePropValue(
                        const_cast<vehicle_prop_value_t*>(eventData));
            }
            return NO_ERROR;
        }
        ssize_t index = mEventInfos.indexOfKey(eventData->prop);
        if (index < 0) {
            EventInfo info(eventData->timestamp, 1);
            mEventInfos.add(eventData->prop, info);
        } else {
            EventInfo& info = mEventInfos.editValueAt(index);
            info.eventCount++;
            info.lastTimestamp = eventData->timestamp;
        }
        handler = mHandler;
    } while (false);
    //TODO add memory pool
    vehicle_prop_value_t* copy;
    if (doCopy) {
        copy = VehiclePropValueUtil::allocVehiclePropValue(*eventData);
        ASSERT_OR_HANDLE_NO_MEMORY(copy, return NO_MEMORY);
    } else {
        copy = const_cast<vehicle_prop_value_t*>(eventData);
    }
    handler->handleHalEvent(copy);
    return NO_ERROR;
}

status_t VehicleNetworkService::onHalError(int32_t errorCode, int32_t property, int32_t operation,
        bool isInjection) {
    sp<VehicleHalMessageHandler> handler;
    VehicleHalError* error = NULL;
    do {
        Mutex::Autolock autoLock(mLock);
        if (!isInjection) {
            if (mMockingEnabled) {
                // drop real HAL error if mocking is enabled
                return NO_ERROR;
            }
        }

        error = new VehicleHalError(errorCode, property, operation);
        if (error == NULL) {
            return NO_MEMORY;
        }
        handler = mHandler;
    } while (false);
    ALOGI("HAL error, error code:%d, property:0x%x, operation:%d, isInjection:%d",
            errorCode, property, operation, isInjection? 1 : 0);
    handler->handleHalError(error);
    return NO_ERROR;
}

void VehicleNetworkService::dispatchHalEvents(List<vehicle_prop_value_t*>& events) {
    HalClientSpVector activeClients;
    do { // for lock scoping
        Mutex::Autolock autoLock(mLock);
        for (vehicle_prop_value_t* e : events) {
            ssize_t index = mPropertyToClientsMap.indexOfKey(e->prop);
            if (index < 0) {
                EVENT_LOG("HAL event for not subscribed property 0x%x", e->prop);
                continue;
            }
            sp<HalClientSpVector>& clients = mPropertyToClientsMap.editValueAt(index);
            EVENT_LOG("dispatchHalEvents, prop 0x%x, active clients %d", e->prop, clients->size());
            for (size_t i = 0; i < clients->size(); i++) {
                sp<HalClient>& client = clients->editItemAt(i);
                const SubscriptionInfo* info = client->getSubscriptionInfo(e->prop);
                if (SubscribeFlags::HAL_EVENT & info->flags) {
                    activeClients.add(client);
                    client->addEvent(e);
                } else {
                    EVENT_LOG("Client is not subsribed to HAL events, prop: 0x%x", e->prop);
                }
            }
        }
    } while (false);
    EVENT_LOG("dispatchHalEvents num events %d, active clients:%d", events.size(),
            activeClients.size());
    int64_t now = elapsedRealtimeNano();
    for (size_t i = 0; i < activeClients.size(); i++) {
        sp<HalClient> client = activeClients.editItemAt(i);
        client->dispatchEvents(now);
    }
    activeClients.clear();
}

void VehicleNetworkService::dispatchHalError(VehicleHalError* error) {
    List<sp<HalClient> > clientsToDispatch;
    do {
        Mutex::Autolock autoLock(mLock);
        if (error->property != 0) {
            sp<HalClientSpVector> clientsForProperty = findClientsVectorForPropertyLocked(
                    error->property);
            if (clientsForProperty.get() != NULL) {
                for (size_t i = 0; i < clientsForProperty->size(); i++) {
                    sp<HalClient> client = clientsForProperty->itemAt(i);
                    clientsToDispatch.push_back(client);
                }
            }
        }
        // Send to global error handler if property is 0 or if no client subscribing.
        if (error->property == 0 || clientsToDispatch.size() == 0) {
            for (size_t i = 0; i < mBinderToClientMap.size(); i++) {
                sp<HalClient> client = mBinderToClientMap.valueAt(i);
                if (client->isMonitoringHalError()) {
                    clientsToDispatch.push_back(client);
                }
            }
        }
    } while (false);
    ALOGI("dispatchHalError error:%d, property:0x%x, operation:%d, num clients to dispatch:%zu",
            error->errorCode, error->property, error->operation, clientsToDispatch.size());
    for (auto& client : clientsToDispatch) {
        client->dispatchHalError(error->errorCode, error->property, error->operation);
    }
    clientsToDispatch.clear();
}

status_t VehicleNetworkService::loadHal() {
    int r = hw_get_module(VEHICLE_HARDWARE_MODULE_ID, (hw_module_t const**)&mModule);
    if (r != NO_ERROR) {
        ALOGE("cannot load HAL module, error:%d", r);
        return r;
    }
    r = mModule->common.methods->open(&mModule->common, VEHICLE_HARDWARE_DEVICE,
            (hw_device_t**)&mDevice);
    return r;
}

void VehicleNetworkService::closeHal() {
    mDevice->common.close(&mDevice->common);
}
};
