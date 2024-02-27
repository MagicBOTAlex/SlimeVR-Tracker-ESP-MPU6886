#pragma once

#include "../sensor.h"
#include "drivers/lsm6ds3trc.h"
#include "drivers/icm42688p.h"
#include "drivers/mpu6050.h"
#include "../SensorFusionRestDetect.h"

namespace SlimeVR::Sensors
{

template <typename T>
class SoftFusionSensor : public Sensor
{
    using imu = T;
    using i2c = typename imu::i2c;

    static constexpr auto GyroCalibDelaySeconds = 5;
    static constexpr auto GyroCalibSeconds = 3;
    static constexpr auto SampleRateCalibDelaySeconds = 5;
    static constexpr auto SampleRateCalibSeconds = 2;

    static constexpr auto AccelCalibDelaySeconds = 0;
    static constexpr auto AccelCalibRestSeconds = 3;

    static constexpr auto GyroCalibSensitivityDelaySeconds = 3;

    static constexpr double GScale = ((32768. / imu::GyroSensitivity) / 32768.) * (PI / 180.0);
    static constexpr double AScale = CONST_EARTH_GRAVITY / imu::AccelSensitivity;

    static constexpr float GyrOdrMicros = imu::GyrTs * 1e6f;
    static constexpr float AccOdrMicros = imu::AccTs * 1e6f;


    bool detected() const {
        return true;
        const auto value = i2c::readReg(imu::Regs::WhoAmI::reg);
		if (imu::Regs::WhoAmI::value != value) {
			m_Logger.error("Sensor not detected, expected reg 0x%02x = 0x%02x but got 0x%02x",
                imu::Regs::WhoAmI::reg, imu::Regs::WhoAmI::value, value);
            return false;
		}

		return true;
    }


    void sendTempIfNeeded() {
        uint32_t now = micros();
        constexpr float maxSendRateHz = 2.0f;
        constexpr uint32_t sendInterval = 1.0f/maxSendRateHz * 1e6;
        uint32_t elapsed = now - m_lastTemperaturePacketSent;
        if (elapsed >= sendInterval) {
            const float temperature = m_sensor.getDirectTemp();
            m_lastTemperaturePacketSent = now - (elapsed - sendInterval);
            networkConnection.sendTemperature(sensorId, temperature);
            //m_Logger.warn("Rest %d", m_fusion.getRestDetected());
        }
    }

    void recalcFusion()
    {
        m_fusion = SensorFusionRestDetect(m_calibration.G_Ts, m_calibration.A_Ts, m_calibration.M_Ts);
    }

    void processAccelSample(const int16_t xyz[3], const sensor_real_t timeDelta)
    {
        sensor_real_t accelData[] = {
            static_cast<sensor_real_t>(xyz[0]),
            static_cast<sensor_real_t>(xyz[1]),
            static_cast<sensor_real_t>(xyz[2]) };

		float tmp[3];
        for (uint8_t i = 0; i < 3; i++)
            tmp[i] = (accelData[i] - m_calibration.A_B[i]);

        accelData[0] = (m_calibration.A_Ainv[0][0] * tmp[0] + m_calibration.A_Ainv[0][1] * tmp[1] + m_calibration.A_Ainv[0][2] * tmp[2]) * AScale;
        accelData[1] = (m_calibration.A_Ainv[1][0] * tmp[0] + m_calibration.A_Ainv[1][1] * tmp[1] + m_calibration.A_Ainv[1][2] * tmp[2]) * AScale;
        accelData[2] = (m_calibration.A_Ainv[2][0] * tmp[0] + m_calibration.A_Ainv[2][1] * tmp[1] + m_calibration.A_Ainv[2][2] * tmp[2]) * AScale;

        m_fusion.updateAcc(accelData, m_calibration.A_Ts);
    }

    void processGyroSample(const int16_t xyz[3], const sensor_real_t timeDelta)
    {
        const sensor_real_t scaledData[] = {
            static_cast<sensor_real_t>(GScale * (static_cast<sensor_real_t>(xyz[0]) - m_calibration.G_off[0])),
            static_cast<sensor_real_t>(GScale * (static_cast<sensor_real_t>(xyz[1]) - m_calibration.G_off[1])),
            static_cast<sensor_real_t>(GScale * (static_cast<sensor_real_t>(xyz[2]) - m_calibration.G_off[2]))};
        m_fusion.updateGyro(scaledData, m_calibration.G_Ts);
    }

    void eatSamplesForSeconds(const size_t seconds) {
        const auto calibTargetDelay = millis() + 1000 * seconds;
        auto lastSecondsRemaining = seconds;
        while (millis() < calibTargetDelay)
        {
            #ifdef ESP8266
                ESP.wdtFeed();
            #endif
            auto currentSecondsRemaining = (calibTargetDelay - millis()) / 1000;
            if (currentSecondsRemaining != lastSecondsRemaining) {
                m_Logger.info("%d...", currentSecondsRemaining + 1);
                lastSecondsRemaining = currentSecondsRemaining;
            }
            m_sensor.bulkRead(
                [](const int16_t xyz[3], const sensor_real_t timeDelta) { },
                [](const int16_t xyz[3], const sensor_real_t timeDelta) { }
            );
        }
    }

public:
    SoftFusionSensor(uint8_t id, uint8_t sclPin, uint8_t sdaPin, float rotation)
    : Sensor(imu::Name, imu::Type, id, imu::DevAddr, rotation, sclPin, sdaPin),
      m_fusion(imu::GyrTs, imu::AccTs, imu::MagTs) {}
    ~SoftFusionSensor(){}

    void motionLoop() override final
    {
        sendTempIfNeeded();

        // read fifo updating fusion
        uint32_t now = micros();
        constexpr uint32_t targetPollIntervalMicros = 6000;
        uint32_t elapsed = now - m_lastPollTime;
        if (elapsed >= targetPollIntervalMicros) {
            m_lastPollTime = now - (elapsed - targetPollIntervalMicros);
            m_sensor.bulkRead(
                [&](const int16_t xyz[3], const sensor_real_t timeDelta) { processAccelSample(xyz, timeDelta);},
                [&](const int16_t xyz[3], const sensor_real_t timeDelta) { processGyroSample(xyz, timeDelta);}
            );
            optimistic_yield(100);
            if (!m_fusion.isUpdated()) return;
            m_fusion.clearUpdated();
        }


        // send new fusion values when time is up
        now = micros();
        constexpr float maxSendRateHz = 120.0f;
        constexpr uint32_t sendInterval = 1.0f/maxSendRateHz * 1e6;
        elapsed = now - m_lastRotationPacketSent;
        if (elapsed >= sendInterval) {
            m_lastRotationPacketSent = now - (elapsed - sendInterval);

            fusedRotation = m_fusion.getQuaternionQuat();
            setFusedRotationReady();

            acceleration = m_fusion.getLinearAccVec();
            setAccelerationReady();

            fusedRotation *= sensorOffset;
            optimistic_yield(100);
        }

    }

    void motionSetup() override final
    {
        if (!detected()) {
            m_status = SensorStatus::SENSOR_ERROR;
            return;
        }

        if (!m_sensor.initialize()) {
            m_Logger.error("Sensor failed to initialize!");
            m_status = SensorStatus::SENSOR_ERROR;
            return;
        }

        SlimeVR::Configuration::CalibrationConfig sensorCalibration = configuration.getCalibration(sensorId);
        // If no compatible calibration data is found, the calibration data will just be zeroed out
        switch (sensorCalibration.type) {
            case SlimeVR::Configuration::CalibrationConfigType::SFUSION:
                m_calibration = sensorCalibration.data.sfusion;
                recalcFusion();
                break;

            case SlimeVR::Configuration::CalibrationConfigType::NONE:
                m_Logger.warn("No calibration data found for sensor %d, ignoring...", sensorId);
                m_Logger.info("Calibration is advised");
            // passtrough
            default:
                m_Logger.warn("Incompatible calibration data found for sensor %d, ignoring...", sensorId);
                m_Logger.info("Calibration is advised");
                startCalibration(0);
        }

		m_status = SensorStatus::SENSOR_OK;
		
		working = true;

        calibrateGyroSensitivity();
    }


    void startCalibration(int calibrationType) override final
    {
        if (calibrationType == 0) {
            // ALL
            calibrateSampleRate();
            calibrateGyroOffset();
            calibrateAccel();
            calibrateGyroSensitivity();
        }
        else if (calibrationType == 1)
        {
            calibrateSampleRate();
        }
        else if (calibrationType == 2)
        {
            calibrateGyroOffset();
        }
        else if (calibrationType == 3)
        {
            calibrateAccel();
        }
        else if (calibrationType == 4) {
            calibrateGyroSensitivity();
        }

        saveCalibration();
    }

    void saveCalibration()
    {
        m_Logger.debug("Saving the calibration data");
        SlimeVR::Configuration::CalibrationConfig calibration;
        calibration.type = SlimeVR::Configuration::CalibrationConfigType::SFUSION;
        calibration.data.sfusion = m_calibration;
        configuration.setCalibration(sensorId, calibration);
        configuration.save();
    }

    void calibrateGyroOffset()
    {
		// Wait for sensor to calm down before calibration
        m_Logger.info("Put down the device and wait for baseline gyro reading calibration (%d seconds)", GyroCalibDelaySeconds);
        ledManager.on();
        eatSamplesForSeconds(GyroCalibDelaySeconds);
        ledManager.off();

        m_calibration.temperature = m_sensor.getDirectTemp();
        m_Logger.trace("Calibration temperature: %f", m_calibration.temperature);

        ledManager.pattern(100, 100, 3);
        ledManager.on();
        m_Logger.info("Gyro calibration started...");

        int32_t sumXYZ[3] = {0};
        const auto targetCalib = millis() + 1000 * GyroCalibSeconds;
        uint32_t sampleCount = 0;

        while (millis() < targetCalib)
        {
            m_sensor.bulkRead(
                [](const int16_t xyz[3], const sensor_real_t timeDelta) { },
                [&sumXYZ, &sampleCount](const int16_t xyz[3], const sensor_real_t timeDelta) {
                    sumXYZ[0] += xyz[0];
                    sumXYZ[1] += xyz[1];
                    sumXYZ[2] += xyz[2];
                    ++sampleCount;
                }
            );
        }

        ledManager.off();
        m_calibration.G_off[0] = ((double)sumXYZ[0]) / sampleCount;
        m_calibration.G_off[1] = ((double)sumXYZ[1]) / sampleCount;
        m_calibration.G_off[2] = ((double)sumXYZ[2]) / sampleCount;

        m_Logger.info("Gyro offset after %d samples: %f %f %f", sampleCount, UNPACK_VECTOR_ARRAY(m_calibration.G_off));
    }

    void calibrateAccel()
    {
        m_Logger.info("Skipping accel cali");

        auto magneto = std::make_unique<MagnetoCalibration>();
        m_Logger.info("Put the device into 6 unique orientations (all sides), leave it still and do not hold/touch for %d seconds each", AccelCalibRestSeconds);
        ledManager.on();
        eatSamplesForSeconds(AccelCalibDelaySeconds);
        ledManager.off();

        RestDetectionParams calibrationRestDetectionParams;
        calibrationRestDetectionParams.restMinTimeMicros = AccelCalibRestSeconds * 1e6;
        calibrationRestDetectionParams.restThAcc = 6.0f;

		RestDetection calibrationRestDetection(
            calibrationRestDetectionParams,
            imu::GyrTs,
            imu::AccTs
        );

		constexpr uint16_t expectedPositions = 6;
        constexpr uint16_t numSamplesPerPosition = 96;

        uint16_t numPositionsRecorded = 0;
        uint16_t numCurrentPositionSamples = 0;
        bool waitForMotion = true;

		float* accelCalibrationChunk = new float[numSamplesPerPosition * 3];
        ledManager.pattern(100, 100, 6);
        ledManager.on();
        m_Logger.info("Gathering accelerometer data...");
        m_Logger.info("Waiting for position %i, you can leave the device as is...", numPositionsRecorded + 1);
        bool samplesGathered = false;
        while (!samplesGathered) {
            m_sensor.bulkRead(
                [&](const int16_t xyz[3], const sensor_real_t timeDelta) {
                    const sensor_real_t scaledData[] = {
                        static_cast<sensor_real_t>(AScale * static_cast<sensor_real_t>(xyz[0])),
                        static_cast<sensor_real_t>(AScale * static_cast<sensor_real_t>(xyz[1])),
                        static_cast<sensor_real_t>(AScale * static_cast<sensor_real_t>(xyz[2]))};

                    calibrationRestDetection.updateAcc(AccOdrMicros, scaledData);
                    if (waitForMotion) {
                        if (!calibrationRestDetection.getRestDetected()) {
                            waitForMotion = false;
                        }
                        // TODO: delay adjustment if it's needed at all
                        //delayMicroseconds(BMI160_ODR_ACC_MICROS);

                        return;
                    }

                    if (calibrationRestDetection.getRestDetected()) {
                        const uint16_t i = numCurrentPositionSamples * 3;
                        accelCalibrationChunk[i + 0] = xyz[0];
                        accelCalibrationChunk[i + 1] = xyz[1];
                        accelCalibrationChunk[i + 2] = xyz[2];
                        numCurrentPositionSamples++;

                        if (numCurrentPositionSamples >= numSamplesPerPosition) {
                            for (int i = 0; i < numSamplesPerPosition; i++) {
                                magneto->sample(
                                    accelCalibrationChunk[i * 3 + 0],
                                    accelCalibrationChunk[i * 3 + 1],
                                    accelCalibrationChunk[i * 3 + 2]
                                );
                            }
                            numPositionsRecorded++;
                            numCurrentPositionSamples = 0;
                            if (numPositionsRecorded < expectedPositions) {
                                ledManager.pattern(50, 50, 2);
                                ledManager.on();
                                m_Logger.info("Recorded, waiting for position %i...", numPositionsRecorded + 1);
                                waitForMotion = true;
                            }
                        }
                    } else {
                        numCurrentPositionSamples = 0;
                    }

                    if (numPositionsRecorded >= expectedPositions)
                    {
                        samplesGathered = true;
                    }

                    //todo: adjust delay if delay is needed at all
                    //delayMicroseconds(BMI160_ODR_ACC_MICROS);

                },
                [](const int16_t xyz[3], const sensor_real_t timeDelta) { }
            );
        }
        ledManager.off();
        m_Logger.debug("Calculating accelerometer calibration data...");
        delete[] accelCalibrationChunk;

        float A_BAinv[4][3];
        magneto->current_calibration(A_BAinv);

        m_Logger.info("Finished calculating accelerometer calibration");
        m_Logger.debug("Accelerometer calibration matrix:");
        m_Logger.debug("{");
        for (int i = 0; i < 3; i++) {
            m_calibration.A_B[i] = A_BAinv[0][i];
            m_calibration.A_Ainv[0][i] = A_BAinv[1][i];
            m_calibration.A_Ainv[1][i] = A_BAinv[2][i];
            m_calibration.A_Ainv[2][i] = A_BAinv[3][i];
            m_Logger.debug("  %f, %f, %f, %f", A_BAinv[0][i], A_BAinv[1][i], A_BAinv[2][i], A_BAinv[3][i]);
        }
        m_Logger.debug("}");
    }

    void calibrateSampleRate()
    {
        m_Logger.info("Calibrating IMU sample rate in %d second(s)...", SampleRateCalibDelaySeconds);
        ledManager.on();
        eatSamplesForSeconds(SampleRateCalibDelaySeconds);

        uint32_t accelSamples = 0;
        uint32_t gyroSamples = 0;

        const auto calibTarget = millis() + 1000 * SampleRateCalibSeconds;
        m_Logger.debug("Counting samples now...");
        uint32_t currentTime;
        while ((currentTime = millis()) < calibTarget)
        {
            m_sensor.bulkRead(
                [&accelSamples](const int16_t xyz[3], const sensor_real_t timeDelta) { accelSamples++; },
                [&gyroSamples](const int16_t xyz[3], const sensor_real_t timeDelta) { gyroSamples++; }
            );
        }

        const auto millisFromStart = currentTime - (calibTarget - 1000 * SampleRateCalibSeconds);
        m_Logger.debug("Collected %d gyro, %d acc samples during %d ms", gyroSamples, accelSamples, millisFromStart);
        m_calibration.A_Ts = millisFromStart / (accelSamples * 1000.0);
        m_calibration.G_Ts = millisFromStart / (gyroSamples * 1000.0) ;

        m_Logger.debug("Gyro frequency %fHz, accel frequency: %fHz", 1.0/m_calibration.G_Ts, 1.0/m_calibration.A_Ts);
        ledManager.off();

        //fusion needs to be recalculated
        recalcFusion();
    }

    void calibrateGyroSensitivity()
    {
        m_Logger.info("Calibrating IMU sample rate in %d second(s)...", SampleRateCalibDelaySeconds);
        m_Logger.info("Lay your tracker FLAT on flat surface (your desk), WITHOUT strap");
        eatSamplesForSeconds(GyroCalibSensitivityDelaySeconds);
        m_Logger.info("Lean FLAT side of your tracker's case against FLAT object that will NOT move during the calibration");
        m_Logger.info("For example: keyboard, book on your desk, the wall at your desk");
        m_Logger.info("Calibration will start when tracker is put at rest at the correct position");
        uint8_t axisToCalib = 0x7;

        const auto calibTarget = millis() + 1000 * 30;
        uint32_t currentTime;
        bool lastSensorRested = false;
        bool alreadyStarted = false;
        Quat startQuat = {};
        int rotationCount[3] = {0};
        Vector3 previousRotation(0, 0, 0);

        auto handleRotation = [](const float &previous,const float &current, int &count)
        {
            auto absDiff = abs(current-previous);

            if (absDiff > 100) {
                if (current >= 0.0)
                {
                    count += 1;
                }
                else
                {
                    count -= 1;
                }
            }
        };

        while ((currentTime = millis()) < calibTarget)
        {
            // feed the fusion
            m_sensor.bulkRead(
                [&](const int16_t xyz[3], const sensor_real_t timeDelta) { processAccelSample(xyz, timeDelta); },
                [&](const int16_t xyz[3], const sensor_real_t timeDelta) { processGyroSample(xyz, timeDelta); }
            );

            if (m_fusion.getRestDetected()) {
                startQuat = m_fusion.getQuaternionOnlyGyroQuat();
            }
            else {
                Quat endQuat = m_fusion.getQuaternionOnlyGyroQuat();
                constexpr float dpsPerRad = 57.295779578552f;
                auto currentRotation = (endQuat * startQuat.inverse()).get_euler() * dpsPerRad;
                handleRotation(previousRotation.x, currentRotation.x, rotationCount[0]);
                handleRotation(previousRotation.y, currentRotation.y, rotationCount[1]);
                handleRotation(previousRotation.z, currentRotation.z, rotationCount[2]);
                previousRotation = currentRotation;
                m_Logger.info("%f %f %f %d %d %d",
                    currentRotation.x-(360.0*rotationCount[0]),
                    currentRotation.y-(360.0*rotationCount[1]),
                    currentRotation.z-(360.0*rotationCount[2]),
                    rotationCount[0],
                    rotationCount[1],
                    rotationCount[2]);
            }

/*
            if (m_fusion.getRestDetected() && lastSensorRested == false)
            {
                if (alreadyStarted)
                {
                    Quat endQuat = m_fusion.getQuaternionOnlyGyroQuat();
                    constexpr float dpsPerRad = 57.295779578552f;
                    auto rawRotationFinal = (endQuat * startQuat.inverse()).get_euler() * dpsPerRad;
                    m_Logger.info("Initial quat %f %f %f %f", startQuat.x, startQuat.y, startQuat.z, startQuat.w);
                    m_Logger.info("Ending quat %f %f %f %f", endQuat.x, endQuat.y, endQuat.z, endQuat.w);
                    m_Logger.info("Rotation final %f %f %f", rawRotationFinal.x, rawRotationFinal.y, rawRotationFinal.z);
                }

                m_Logger.info("Sensor at rest. Axis remaining to calibrate: %c %c %c",
                    axisToCalib & 0x1 ? 'X' : ' ',
                    axisToCalib & 0x2 ? 'Y' : ' ',
                    axisToCalib & 0x4 ? 'Z' : ' '
                );
                m_Logger.info("Rotate your tracker slowly in ONE of the axis, making full 360 degree rotation.");
                m_Logger.info("You can rotate it multiple times, it's recommeded to do around 3 full rotations.");
                lastSensorRested = true;
                alreadyStarted = true;
                startQuat = m_fusion.getQuaternionOnlyGyroQuat();
            }


            if (!m_fusion.getRestDetected() && lastSensorRested == true)
            {
                lastSensorRested = false;
            }            */
        }

    }

    SensorStatus getSensorState() override final
    {
        return m_status;
    }

    T m_sensor;
    SensorFusionRestDetect m_fusion;
    SlimeVR::Configuration::SoftFusionCalibrationConfig m_calibration = {
        // let's create here transparent calibration that doesn't affect input data
        .A_B = {0.0, 0.0, 0.0},
        .A_Ainv = {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}},
        .M_B = {0.0, 0.0, 0.0},
        .M_Ainv = {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}},
        .G_off = { 0.0, 0.0, 0.0 },
        .temperature = 0.0,
        .A_Ts = imu::AccTs,
        .G_Ts = imu::GyrTs,
        .M_Ts = imu::MagTs,
        .G_Sens = {1.0, 1.0, 1.0}
    };

    SensorStatus m_status = SensorStatus::SENSOR_OFFLINE;
    uint32_t m_lastPollTime = micros();
    uint32_t m_lastRotationPacketSent = 0;
    uint32_t m_lastTemperaturePacketSent = 0;
};

} // namespace
