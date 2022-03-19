#include "soleelliptical.h"

#include "keepawakehelper.h"
#include "virtualtreadmill.h"
#include <QBluetoothLocalDevice>
#include <QDateTime>
#include <QFile>
#include <QMetaEnum>

#include <QSettings>
#include <chrono>

using namespace std::chrono_literals;

soleelliptical::soleelliptical(bool noWriteResistance, bool noHeartService, bool testResistance,
                               uint8_t bikeResistanceOffset, double bikeResistanceGain) {
    m_watt.setType(metric::METRIC_WATT);
    Speed.setType(metric::METRIC_SPEED);
    refresh = new QTimer(this);

    this->testResistance = testResistance;
    this->noWriteResistance = noWriteResistance;
    this->noHeartService = noHeartService;
    this->bikeResistanceGain = bikeResistanceGain;
    this->bikeResistanceOffset = bikeResistanceOffset;

    initDone = false;
    connect(refresh, &QTimer::timeout, this, &soleelliptical::update);
    refresh->start(300ms);
}

soleelliptical::~soleelliptical() {
    qDebug() << QStringLiteral("~soleelliptical()") << virtualTreadmill;
    if (virtualTreadmill) {

        delete virtualTreadmill;
    }
}

void soleelliptical::writeCharacteristic(uint8_t *data, uint8_t data_len, const QString &info, bool disable_log,
                                         bool wait_for_response) {
    QEventLoop loop;
    QTimer timeout;

    if (wait_for_response) {
        connect(gattCommunicationChannelService, &QLowEnergyService::characteristicChanged, &loop, &QEventLoop::quit);
        timeout.singleShot(300ms, &loop, &QEventLoop::quit);
    } else {
        connect(gattCommunicationChannelService, &QLowEnergyService::characteristicWritten, &loop, &QEventLoop::quit);
        timeout.singleShot(300ms, &loop, &QEventLoop::quit);
    }

    gattCommunicationChannelService->writeCharacteristic(gattWriteCharacteristic,
                                                         QByteArray((const char *)data, data_len));

    if (!disable_log) {
        emit debug(QStringLiteral(" >> ") + QByteArray((const char *)data, data_len).toHex(' ') +
                   QStringLiteral(" // ") + info);
    }

    loop.exec();

    if (timeout.isActive() == false) {
        emit debug(QStringLiteral(" exit for timeout"));
    }
}

void soleelliptical::forceResistanceAndInclination(int8_t requestResistance, uint8_t inclination) {

    // TODO do the level down command
    uint8_t write[] = {0x5b, 0x04, 0x00, 0xf1, 0x4f, 0x4b, 0x5d};
    uint8_t writeUp[] = {0x5b, 0x02, 0xf1, 0x02, 0x5d};

    uint8_t writeDown[] = {0x5b, 0x02, 0xf1, 0x03, 0x5d};

    if (currentResistance().value() < requestResistance) {
        writeCharacteristic(write, sizeof(write),
                            QStringLiteral("forceResistance ") + QString::number(requestResistance) +
                                QStringLiteral(" Inclination ") + inclination,
                            false, true);
        writeCharacteristic(writeUp, sizeof(writeUp),
                            QStringLiteral("forceResistance ") + QString::number(requestResistance) +
                                QStringLiteral(" Inclination ") + inclination,
                            false, true);
    } else if (currentResistance().value() > requestResistance) {
        writeCharacteristic(writeDown, sizeof(writeDown),
                            QStringLiteral("forceResistance ") + QString::number(requestResistance) +
                                QStringLiteral(" Inclination ") + inclination,
                            false, true);
        writeCharacteristic(write, sizeof(write),
                            QStringLiteral("forceResistance ") + QString::number(requestResistance) +
                                QStringLiteral(" Inclination ") + inclination,
                            false, true);
    }
}

void soleelliptical::changeInclinationRequested(double grade, double percentage) {
    if (percentage < 0)
        percentage = 0;
    changeInclination(grade, percentage);
}

void soleelliptical::update() {

    uint8_t noOpData[] = {0x5b, 0x04, 0x00, 0x10, 0x4f, 0x4b, 0x5d};
    uint8_t noOpData1[] = {0x5b, 0x04, 0x00, 0x06, 0x4f, 0x4b, 0x5d};

    if (m_control->state() == QLowEnergyController::UnconnectedState) {

        emit disconnected();
        return;
    }

    if (initRequest) {

        initRequest = false;
        btinit(false);
    } else if (bluetoothDevice.isValid() && m_control->state() == QLowEnergyController::DiscoveredState &&
               gattCommunicationChannelService && gattWriteCharacteristic.isValid() &&
               gattNotifyCharacteristic.isValid() && initDone) {

        update_metrics(true, watts());

        QSettings settings;
        // ******************************************* virtual treadmill init *************************************
        if (!firstVirtual && searchStopped && !virtualTreadmill && !virtualBike) {
            bool virtual_device_enabled = settings.value("virtual_device_enabled", true).toBool();
            bool virtual_device_force_bike = settings.value("virtual_device_force_bike", false).toBool();
            if (virtual_device_enabled) {
                if (!virtual_device_force_bike) {
                    debug("creating virtual treadmill interface...");
                    virtualTreadmill = new virtualtreadmill(this, noHeartService);
                    connect(virtualTreadmill, &virtualtreadmill::debug, this, &soleelliptical::debug);
                    connect(virtualTreadmill, &virtualtreadmill::changeInclination, this,
                            &soleelliptical::changeInclinationRequested);
                } else {
                    debug("creating virtual bike interface...");
                    virtualBike = new virtualbike(this);
                    connect(virtualBike, &virtualbike::changeInclination, this,
                            &soleelliptical::changeInclinationRequested);
                }
                firstVirtual = 1;
            }
        }
        // ********************************************************************************************************

        // updating the treadmill console every second
        if (sec1Update++ == (1000 / refresh->interval())) {

            sec1Update = 0;
        } else {
            switch (counterPoll) {

            case 0:
                writeCharacteristic(noOpData, sizeof(noOpData), QStringLiteral("noOp"), false, true);
                break;
            case 1:
                writeCharacteristic(noOpData, sizeof(noOpData1), QStringLiteral("noOp"), false, true);
                break;
            }
            counterPoll++;
            if (counterPoll > 1) {
                counterPoll = 0;
            }
        }

        if (testResistance) {
            if ((((int)elapsed.value()) % 5) == 0) {

                uint8_t new_res = currentResistance().value() + 1;
                if (new_res > 15) {
                    new_res = 1;
                }
                forceResistanceAndInclination(new_res, currentInclination().value());
            }
        }

        // Resistance as incline on Sole E95s Elliptical #419
        if (requestInclination != -1)
            requestResistance = requestInclination;

        if (requestResistance != -1) {
            if (requestResistance > 20) {
                requestResistance = 20;
            } else if (requestResistance == 0) {
                requestResistance = 1;
            }

            if (requestResistance != currentResistance().value()) {
                emit debug(QStringLiteral("writing resistance ") + QString::number(requestResistance));

                forceResistanceAndInclination(requestResistance, currentInclination().value());
            }
            requestResistance = -1;
        } else if (requestInclination != -1) {
            if (requestInclination > 15) {
                requestInclination = 15;
            } else if (requestInclination == 0) {
                requestInclination = 1;
            }

            if (requestInclination != currentInclination().value()) {
                emit debug(QStringLiteral("writing inclination ") + QString::number(requestInclination));

                forceResistanceAndInclination(currentResistance().value(), requestInclination);
            }
            requestInclination = -1;
        }
        if (requestStart != -1) {
            emit debug(QStringLiteral("starting..."));

            btinit(true);

            requestStart = -1;
            emit bikeStarted();
        }
        if (requestStop != -1) {
            emit debug(QStringLiteral("stopping..."));

            // writeCharacteristic(initDataF0C800B8, sizeof(initDataF0C800B8), "stop tape");
            requestStop = -1;
        }
    }
}

void soleelliptical::serviceDiscovered(const QBluetoothUuid &gatt) {
    emit debug(QStringLiteral("serviceDiscovered ") + gatt.toString());
}

void soleelliptical::characteristicChanged(const QLowEnergyCharacteristic &characteristic, const QByteArray &newValue) {

    // qDebug() << "characteristicChanged" << characteristic.uuid() << newValue << newValue.length();
    Q_UNUSED(characteristic);
    QSettings settings;
    QString heartRateBeltName =
        settings.value(QStringLiteral("heart_rate_belt_name"), QStringLiteral("Disabled")).toString();

    emit debug(QStringLiteral(" << ") + newValue.toHex(' '));

    lastPacket = newValue;

    if (newValue.length() == 5 && newValue.at(1) == 0x02) {

        Resistance = newValue.at(3) + 1;
        emit debug(QStringLiteral("Current resistance: ") + QString::number(Resistance.value()));
        return;
    }

    if (newValue.length() < 20) {
        return;
    }

    double speed =
        GetSpeedFromPacket(newValue) * settings.value(QStringLiteral("domyos_elliptical_speed_ratio"), 1.0).toDouble();
    double kcal = GetKcalFromPacket(newValue);
    // double distance = GetDistanceFromPacket(newValue) *
    // settings.value("domyos_elliptical_speed_ratio", 1.0).toDouble();
    uint16_t watt = (newValue.at(13) << 8) | newValue.at(14);

    if (settings.value(QStringLiteral("cadence_sensor_name"), QStringLiteral("Disabled"))
            .toString()
            .startsWith(QStringLiteral("Disabled"))) {
        Cadence = ((uint8_t)newValue.at(10));
    }
    m_watt = watt;

    // Inclination = newValue.at(21);
    if (Resistance.value() < 1) {
        emit debug(QStringLiteral("invalid resistance value ") + QString::number(Resistance.value()) +
                   QStringLiteral(" putting to default"));
        Resistance = 1;
    }

#ifdef Q_OS_ANDROID
    if (settings.value("ant_heart", false).toBool())
        Heart = (uint8_t)KeepAwakeHelper::heart();
    else
#endif
    {
        if (heartRateBeltName.startsWith(QStringLiteral("Disabled"))) {
            Heart = ((uint8_t)newValue.at(18));
        }
    }

    Distance += ((Speed.value() / 3600000.0) *
                 ((double)lastRefreshCharacteristicChanged.msecsTo(QDateTime::currentDateTime())));

    CrankRevs++;
    LastCrankEventTime += (uint16_t)(1024.0 / (((double)(Cadence.value())) / 60.0));
    lastRefreshCharacteristicChanged = QDateTime::currentDateTime();

    emit debug(QStringLiteral("Current speed: ") + QString::number(speed));
    emit debug(QStringLiteral("Current cadence: ") + QString::number(Cadence.value()));
    emit debug(QStringLiteral("Current inclination: ") + QString::number(Inclination.value()));
    emit debug(QStringLiteral("Current heart: ") + QString::number(Heart.value()));
    emit debug(QStringLiteral("Current KCal: ") + QString::number(kcal));
    emit debug(QStringLiteral("Current Distance: ") + QString::number(Distance.value()));
    emit debug(QStringLiteral("Current CrankRevs: ") + QString::number(CrankRevs));
    emit debug(QStringLiteral("Last CrankEventTime: ") + QString::number(LastCrankEventTime));
    emit debug(QStringLiteral("Current Watt: ") + QString::number(watts()));

    if (m_control->error() != QLowEnergyController::NoError) {
        qDebug() << QStringLiteral("QLowEnergyController ERROR!!") << m_control->errorString();
    }

    Speed = speed;
    KCal = kcal;
}

double soleelliptical::GetSpeedFromPacket(const QByteArray &packet) {

    uint16_t convertedData = (packet.at(11) << 8) | ((uint8_t)packet.at(12));
    double data = (double)convertedData / 100.0f;
    return data;
}

double soleelliptical::GetKcalFromPacket(const QByteArray &packet) {

    uint16_t convertedData = (packet.at(7) << 8) | ((uint8_t)packet.at(8));
    return (double)convertedData / 10.0;
}

double soleelliptical::GetDistanceFromPacket(const QByteArray &packet) {

    uint16_t convertedData = (packet.at(12) << 8) | packet.at(13);
    double data = ((double)convertedData) / 10.0f;
    return data;
}

void soleelliptical::btinit(bool startTape) {

    QSettings settings;
    Q_UNUSED(startTape)

    // set speed and incline to 0
    uint8_t initData1[] = {0x5b, 0x01, 0xf0, 0x5d};
    uint8_t initData2[] = {0x5b, 0x02, 0x03, 0x01, 0x5d};
    uint8_t initData3[] = {0x5b, 0x06, 0x07, 0x01, 0x23, 0x00, 0x9b, 0xaa, 0x5d};
    uint8_t initData4[] = {0x5b, 0x03, 0x08, 0x10, 0x01, 0x5d};
    uint8_t initData5[] = {0x5b, 0x05, 0x04, 0xFF, 0x00, 0x00, 0x00, 0x5d}; // 0xFF is the duration of the workout
    uint8_t initData6[] = {0x5b, 0x02, 0x02, 0x02, 0x5d};
    uint8_t initData7[] = {0x5b, 0x02, 0x03, 0x04, 0x5d};

    initData3[4] = settings.value(QStringLiteral("age"), 35).toUInt();
    initData3[6] = settings.value(QStringLiteral("weight"), 75.0).toFloat() * 2.20462;

    writeCharacteristic(initData1, sizeof(initData1), QStringLiteral("init"));
    writeCharacteristic(initData1, sizeof(initData1), QStringLiteral("init"));
    writeCharacteristic(initData1, sizeof(initData1), QStringLiteral("init"));
    writeCharacteristic(initData2, sizeof(initData2), QStringLiteral("init"));
    writeCharacteristic(initData2, sizeof(initData2), QStringLiteral("init"));
    writeCharacteristic(initData3, sizeof(initData3), QStringLiteral("init"));
    writeCharacteristic(initData4, sizeof(initData4), QStringLiteral("init"));
    writeCharacteristic(initData5, sizeof(initData5), QStringLiteral("init"));
    writeCharacteristic(initData6, sizeof(initData6), QStringLiteral("init"));
    writeCharacteristic(initData7, sizeof(initData7), QStringLiteral("init"));

    initDone = true;
}

void soleelliptical::stateChanged(QLowEnergyService::ServiceState state) {
    QBluetoothUuid _gattWriteCharacteristicId(QStringLiteral("49535343-8841-43f4-a8d4-ecbe34729bb3"));
    QBluetoothUuid _gattNotifyCharacteristicId(QStringLiteral("49535343-1e4d-4bd9-ba61-23c647249616"));

    QMetaEnum metaEnum = QMetaEnum::fromType<QLowEnergyService::ServiceState>();
    emit debug(QStringLiteral("BTLE stateChanged ") + QString::fromLocal8Bit(metaEnum.valueToKey(state)));

    if (state == QLowEnergyService::ServiceDiscovered) {

        // qDebug() << gattCommunicationChannelService->characteristics();

        gattWriteCharacteristic = gattCommunicationChannelService->characteristic(_gattWriteCharacteristicId);
        gattNotifyCharacteristic = gattCommunicationChannelService->characteristic(_gattNotifyCharacteristicId);
        Q_ASSERT(gattWriteCharacteristic.isValid());
        Q_ASSERT(gattNotifyCharacteristic.isValid());

        // establish hook into notifications
        connect(gattCommunicationChannelService, &QLowEnergyService::characteristicChanged, this,
                &soleelliptical::characteristicChanged);
        connect(gattCommunicationChannelService, &QLowEnergyService::characteristicWritten, this,
                &soleelliptical::characteristicWritten);
        connect(gattCommunicationChannelService,
                static_cast<void (QLowEnergyService::*)(QLowEnergyService::ServiceError)>(&QLowEnergyService::error),
                this, &soleelliptical::errorService);
        connect(gattCommunicationChannelService, &QLowEnergyService::descriptorWritten, this,
                &soleelliptical::descriptorWritten);

        QByteArray descriptor;
        descriptor.append((char)0x01);
        descriptor.append((char)0x00);
        gattCommunicationChannelService->writeDescriptor(
            gattNotifyCharacteristic.descriptor(QBluetoothUuid::ClientCharacteristicConfiguration), descriptor);
    }
}

void soleelliptical::searchingStop() { searchStopped = true; }

void soleelliptical::descriptorWritten(const QLowEnergyDescriptor &descriptor, const QByteArray &newValue) {
    emit debug(QStringLiteral("descriptorWritten ") + descriptor.name() + QStringLiteral(" ") + newValue.toHex(' '));

    initRequest = true;
    emit connectedAndDiscovered();
}

void soleelliptical::characteristicWritten(const QLowEnergyCharacteristic &characteristic, const QByteArray &newValue) {

    Q_UNUSED(characteristic);
    emit debug(QStringLiteral("characteristicWritten ") + newValue.toHex(' '));
}

void soleelliptical::serviceScanDone(void) {
    emit debug(QStringLiteral("serviceScanDone"));

    QBluetoothUuid _gattCommunicationChannelServiceId(QStringLiteral("49535343-fe7d-4ae5-8fa9-9fafd205e455"));

    gattCommunicationChannelService = m_control->createServiceObject(_gattCommunicationChannelServiceId);
    connect(gattCommunicationChannelService, &QLowEnergyService::stateChanged, this, &soleelliptical::stateChanged);
    gattCommunicationChannelService->discoverDetails();
}

void soleelliptical::errorService(QLowEnergyService::ServiceError err) {

    QMetaEnum metaEnum = QMetaEnum::fromType<QLowEnergyService::ServiceError>();
    emit debug(QStringLiteral("soleelliptical::errorService") + QString::fromLocal8Bit(metaEnum.valueToKey(err)) +
               m_control->errorString());
}

void soleelliptical::error(QLowEnergyController::Error err) {

    QMetaEnum metaEnum = QMetaEnum::fromType<QLowEnergyController::Error>();
    emit debug(QStringLiteral("soleelliptical::error") + QString::fromLocal8Bit(metaEnum.valueToKey(err)) +
               m_control->errorString());
}

void soleelliptical::deviceDiscovered(const QBluetoothDeviceInfo &device) {
    emit debug(QStringLiteral("Found new device: ") + device.name() + " (" + device.address().toString() + ')');

    {
        bluetoothDevice = device;

        m_control = QLowEnergyController::createCentral(bluetoothDevice, this);
        connect(m_control, &QLowEnergyController::serviceDiscovered, this, &soleelliptical::serviceDiscovered);
        connect(m_control, &QLowEnergyController::discoveryFinished, this, &soleelliptical::serviceScanDone);
        connect(m_control,
                static_cast<void (QLowEnergyController::*)(QLowEnergyController::Error)>(&QLowEnergyController::error),
                this, &soleelliptical::error);
        connect(m_control, &QLowEnergyController::stateChanged, this, &soleelliptical::controllerStateChanged);

        connect(m_control,
                static_cast<void (QLowEnergyController::*)(QLowEnergyController::Error)>(&QLowEnergyController::error),
                this, [this](QLowEnergyController::Error error) {
                    Q_UNUSED(error);
                    Q_UNUSED(this);
                    emit debug(QStringLiteral("Cannot connect to remote device."));
                    searchStopped = false;
                    emit disconnected();
                });
        connect(m_control, &QLowEnergyController::connected, this, [this]() {
            Q_UNUSED(this);
            emit debug(QStringLiteral("Controller connected. Search services..."));
            m_control->discoverServices();
        });
        connect(m_control, &QLowEnergyController::disconnected, this, [this]() {
            Q_UNUSED(this);
            emit debug(QStringLiteral("LowEnergy controller disconnected"));
            searchStopped = false;
            emit disconnected();
        });

        // Connect
        m_control->connectToDevice();
        return;
    }
}

bool soleelliptical::connected() {
    if (!m_control) {

        return false;
    }
    return m_control->state() == QLowEnergyController::DiscoveredState;
}

void *soleelliptical::VirtualTreadmill() { return virtualTreadmill; }

void *soleelliptical::VirtualDevice() { return VirtualTreadmill(); }

uint16_t soleelliptical::watts() { return m_watt.value(); }

void soleelliptical::controllerStateChanged(QLowEnergyController::ControllerState state) {
    qDebug() << QStringLiteral("controllerStateChanged") << state;
    if (state == QLowEnergyController::UnconnectedState && m_control) {
        qDebug() << QStringLiteral("trying to connect back again...");

        initDone = false;
        m_control->connectToDevice();
    }
}
