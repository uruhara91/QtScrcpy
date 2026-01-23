#include <QDebug>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

#include "devicemanage.h"
#include "device.h"
#include "demuxer.h"

namespace qsc {

#define DM_MAX_DEVICES_NUM 1000

IDeviceManage& IDeviceManage::getInstance() {
    static DeviceManage dm;
    return dm;
}

DeviceManage::DeviceManage() {
    if (!Demuxer::init()) {
        qCritical("Demuxer init failed!");
    }
}

DeviceManage::~DeviceManage() {
    Demuxer::deInit();
}

QPointer<IDevice> DeviceManage::getDevice(const QString &serial)
{
    if (!m_devices.contains(serial)) {
        return QPointer<IDevice>();
    }
    return m_devices[serial];
}

bool DeviceManage::connectDevice(qsc::DeviceParams params)
{
    if (params.serial.trimmed().isEmpty()) {
        return false;
    }
    if (m_devices.contains(params.serial)) {
        return false;
    }
    if (DM_MAX_DEVICES_NUM < m_devices.size()) {
        qInfo("over the maximum number of connections");
        return false;
    }
    
    IDevice *device = new Device(params);
    connect(device, &Device::deviceConnected, this, &DeviceManage::onDeviceConnected);
    connect(device, &Device::deviceDisconnected, this, &DeviceManage::onDeviceDisconnected);
    
    if (!device->connectDevice()) {
        delete device;
        return false;
    }
    m_devices[params.serial] = device;
    return true;
}

bool DeviceManage::disconnectDevice(const QString &serial)
{
    bool ret = false;
    if (!serial.isEmpty() && m_devices.contains(serial)) {
        auto it = m_devices.find(serial);
        if (!it.value().isNull()) {
            delete it.value().data();
            ret = true;
        }
    }
    return ret;
}

void DeviceManage::disconnectAllDevice()
{
    QMapIterator<QString, QPointer<IDevice>> i(m_devices);
    while (i.hasNext()) {
        i.next();
        if (!i.value().isNull()) {
            delete i.value().data();
        }
    }
}

void DeviceManage::onDeviceConnected(bool success, const QString &serial, const QString &deviceName, const QSize &size)
{
    emit deviceConnected(success, serial, deviceName, size);
    if (!success) {
        removeDevice(serial);
    }
}

void DeviceManage::onDeviceDisconnected(QString serial)
{
    emit deviceDisconnected(serial);
    removeDevice(serial);
}

quint16 DeviceManage::getFreePort()
{
    quint16 port = m_localPortStart;
    while (port < m_localPortStart + DM_MAX_DEVICES_NUM) {
        bool used = false;
        QMapIterator<QString, QPointer<IDevice>> i(m_devices);
        while (i.hasNext()) {
            i.next();
            auto device = i.value();
            if (!device.isNull() && device->isReversePort(port)) {
                used = true;
                break;
            }
        }
        if (!used) {
            return port;
        }
        port++;
    }
    return 0;
}

void DeviceManage::removeDevice(const QString &serial)
{
    if (!serial.isEmpty() && m_devices.contains(serial)) {
        if (!m_devices[serial].isNull()) {
            m_devices[serial]->deleteLater();
        }
        m_devices.remove(serial);
    }
}

}