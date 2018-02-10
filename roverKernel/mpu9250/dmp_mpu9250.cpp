/**
 *  mpu9250.cpp
 *
 *  Created on: 25. 3. 2015.
 *      Author: Vedran
 */
#include "mpu9250.h"

#if defined(__HAL_USE_MPU9250_DMP__)       //  Compile only if module is enabled

#include "HAL/hal.h"
#include "libs/myLib.h"
#include "libs/helper_3dmath.h"

#include "eMPL/inv_mpu.h"
#include "eMPL/inv_mpu_dmp_motion_driver.h"

#include "registerMap.h"

//  Enable debug information printed on serial port
#define __DEBUG_SESSION__

//  Sensitivity of sensor while outputting quaternions
#define QUAT_SENS  1073741824.0f

//  Integration with event log, if it's present
#ifdef __HAL_USE_EVENTLOG__
    #include "init/eventLog.h"
    //  Simplify emitting events
    #define EMIT_EV(X, Y)  EventLog::EmitEvent(MPU_UID, X, Y)
#endif  /* __HAL_USE_EVENTLOG__ */

#ifdef __DEBUG_SESSION__
#include "serialPort/uartHW.h"
#endif

//  Function prototype to an interrupt handler (declared at the bottom)
void MPUDataHandler(void);

///-----------------------------------------------------------------------------
///         DMP related structs --  Start
///-----------------------------------------------------------------------------

/* The sensors can be mounted onto the board in any orientation. The mounting
 * matrix seen below tells the MPL how to rotate the raw data from thei
 * driver(s).
 * TODO: The following matrices refer to the configuration on an internal test
 * board at Invensense. If needed, please modify the matrices to match the
 * chip-to-body matrix for your particular set up.
 */
//  In this case chip is rotated -90° around Y axis
//static signed char gyro_orientation[9] = { 0, 0, -1,
//                                           0, 1,  0,
//                                           1, 0,  0};
static signed char gyro_orientation[9] = { 1, 0, 0,
                                           0, 1,  0,
                                           0, 0,  1};


struct int_param_s int_param;

struct rx_s {
    unsigned char header[3];
    unsigned char cmd;
};
struct hal_s {
    unsigned char sensors;
    unsigned char dmp_on;
    unsigned char wait_for_tap;
    volatile unsigned char new_gyro;
    unsigned short report;
    unsigned short dmp_features;
    unsigned char motion_int_mode;
    struct rx_s rx;
};
static struct hal_s hal = {0};

///-----------------------------------------------------------------------------
///         DMP related structs --  End
///-----------------------------------------------------------------------------

///-----------------------------------------------------------------------------
///         DMP related functions --  Start
///-----------------------------------------------------------------------------

/* These next two functions converts the orientation matrix (see
 * gyro_orientation) to a scalar representation for use by the DMP.
 * NOTE: These functions are borrowed from Invensense's MPL.
 */
static inline unsigned short inv_row_2_scale(const signed char *row)
{
    unsigned short b;

    if (row[0] > 0)
        b = 0;
    else if (row[0] < 0)
        b = 4;
    else if (row[1] > 0)
        b = 1;
    else if (row[1] < 0)
        b = 5;
    else if (row[2] > 0)
        b = 2;
    else if (row[2] < 0)
        b = 6;
    else
        b = 7;      // error
    return b;
}

static inline unsigned short inv_orientation_matrix_to_scalar(
    const signed char *mtx)
{
    unsigned short scalar;

    /*
       XYZ  010_001_000 Identity Matrix
       XZY  001_010_000
       YXZ  010_000_001
       YZX  000_010_001
       ZXY  001_000_010
       ZYX  000_001_010
     */

    scalar = inv_row_2_scale(mtx);
    scalar |= inv_row_2_scale(mtx + 3) << 3;
    scalar |= inv_row_2_scale(mtx + 6) << 6;


    return scalar;
}

///-----------------------------------------------------------------------------
///         DMP related functions --  End
///-----------------------------------------------------------------------------

#if defined(__USE_TASK_SCHEDULER__)

/**
 * Callback routine to invoke service offered by this module from task scheduler
 * @note It is assumed that once this function is called task scheduler has
 * already copied required variables into the memory space provided for it.
 */
void _MPU_KernelCallback(void)
{
    MPU9250 &__mpu = MPU9250::GetI();

    static float sumOfRot = 0;


    //  Check for null-pointer
    if (__mpu._ker.args == 0)
        return;

    /*
     *  Data in args[] contains bytes that constitute arguments for function
     *  calls. The exact representation(i.e. whether bytes represent ints, floats)
     *  of data is known only to individual blocks of switch() function. There
     *  is no predefined data separator between arguments inside args[].
     */
    switch (__mpu._ker.serviceID)
    {
    /*
     * Change state of the power switch. Allows for powering down MPU chip
     * args[] = powerState(bool)
     * retVal one of MPU_* error codes
     */
    case MPU_T_POWERSW:
        {
            //  Double negation to convert any non-zero int to bool
            bool powerState = !(!(__mpu._ker.args[0]));

            HAL_MPU_PowerSwitch(powerState);

            //  If sensor is powering on reset I2C and load DMP firmware
            if (powerState)
            {
                __mpu.InitHW();
                __mpu.InitSW();
            }
            __mpu._ker.retVal = MPU_SUCCESS;
        }
        break;

    /*
     *  Once new data is available, read it from FIFO and store in data structure
     *  args[] = none
     *  retVal one of MPU_* error codes
     */
    case MPU_T_GET_DATA:
        {
            static bool suppressError = false;

//#ifdef __DEBUG_SESSION__
//    DEBUG_WRITE("In ISR\n");
//    uint8_t id = (uint8_t)HAL_MPU_ReadByte(0x00, 117);
//    DEBUG_WRITE("  My ID is: %d\n", id);
//    return;
//#endif
//    return;

            if (__mpu.IsDataReady())
            {
                int8_t retVal;
                short gyro[3], accel[3], sensors;
                unsigned char more = 1;
                long quat[4];
                unsigned long sensor_timestamp;

            #ifdef __USE_TASK_SCHEDULER__
                //  Calculate dT in seconds!
                static uint64_t oldms = 0;
                __mpu.dT = (float)(msSinceStartup-oldms)/1000.0f;
                oldms = msSinceStartup;
            #endif /* __HAL_USE_TASKSCH__ */

                /* This function gets new data from the FIFO when the DMP is in
                 * use. The FIFO can contain any combination of gyro, accel,
                 * quaternion, and gesture data. The sensors parameter tells the
                 * caller which data fields were actually populated with new data.
                 * For example, if sensors == (INV_XYZ_GYRO | INV_WXYZ_QUAT), then
                 * the FIFO isn't being filled with accel data.
                 * The driver parses the gesture data to determine if a gesture
                 * event has occurred; on an event, the application will be notified
                 * via a callback (assuming that a callback function was properly
                 * registered). The more parameter is non-zero if there are
                 * leftover packets in the FIFO.
                 */
                int cnt = 0;
                //  Make sure the fifo is empty before leaving this loop, in
                //  order to prevent fifo overflow on consecutive sensor reading
                while (cnt < 100)   //Read max 100 packets, if there's more we
                                    //   have a problem
                {
                    retVal = dmp_read_fifo(gyro, accel, quat, &sensor_timestamp, &sensors, &more);
                    cnt++;

                    if (retVal == (-2))
                        DEBUG_WRITE("READ_FIFO returned: %d \n", retVal);

                    if (sensors == 0)   //No data available
                    {
                        __mpu._ker.retVal = MPU_SUCCESS;
                        break;
                    }

                    //  If reading fifo returned error, move to next packet
                    if (retVal)
                        continue;

                    //  If there was no error, extract orientation data
                    Quaternion qt;
                    qt.x = (float)quat[0]/QUAT_SENS;
                    qt.y = (float)quat[1]/QUAT_SENS;
                    qt.z = (float)quat[2]/QUAT_SENS;
                    qt.w = (float)quat[3]/QUAT_SENS;

                    VectorFloat v;
                    dmp_GetGravity(&v, &qt);

                    dmp_GetYawPitchRoll((float*)(__mpu._ypr), &qt, &v);

                    __mpu._quat[0] = qt.x;
                    __mpu._quat[1] = qt.y;
                    __mpu._quat[2] = qt.z;
                    __mpu._quat[3] = qt.w;

                    //  Copy to MPU class
                    __mpu._gv[0] = v.x;
                    __mpu._gv[1] = v.y;
                    __mpu._gv[2] = v.z;

                    __mpu._acc[0] = (float)accel[0]/32767.0;
                    __mpu._acc[1] = (float)accel[1]/32767.0;
                    __mpu._acc[2] = (float)accel[2]/32767.0;
                }

                //  If there was only one packet in FIFO, and it caused error,
                //  then emit hang
                if ((retVal != 0) && (cnt == 1) && (sensors != 0))
                {
                    //  Use emitting event to also report error code through
                    //  taskID parameter
            #ifdef __HAL_USE_EVENTLOG__
                    EMIT_EV(retVal, EVENT_HANG);
            #endif  /* __HAL_USE_EVENTLOG__ */
                    return;
                }

                //  Do data health-check -> too big change in angle(30° cumulative)
                //  between consecutive readings points to error
                if ((fabs(sumOfRot - fabs(__mpu._ypr[0]) - fabs(__mpu._ypr[1]) -
                          fabs(__mpu._ypr[2])) > 0.5) && (sumOfRot != 0.0))
                {
                    //  Emit error event to the system if consecutive sensor
                    //   readings are too far off and suppress
                    if (!suppressError)
                    {
#ifdef __HAL_USE_EVENTLOG__
                    EMIT_EV(MPU_T_GET_DATA, EVENT_ERROR);
#endif  /* __HAL_USE_EVENTLOG__ */
                    __mpu._ker.retVal = MPU_ERROR;
                    }
                }
                else
                {
                    __mpu._ker.retVal = MPU_SUCCESS;
                }

                //  Update sum of rotations for next function call
                sumOfRot = fabs(__mpu._ypr[0]) + fabs(__mpu._ypr[1]) + fabs(__mpu._ypr[2]);
                suppressError = false;
            }
            else
                //  When not listening to sensor readings for a while, it is
                //  possible to have a big change in readings when starting to
                //  listen again, in that case first error message is suppressed
                suppressError = true;

        }
        break;
        /*
         * Restart MPU module and reload DMP firmware
         * args[] = rebootCode(0x17)
         * retVal one of MPU_* error codes
         */
    case MPU_T_REBOOT:
        {
            if (__mpu._ker.args[0] == 0x17)
            {
                sumOfRot = 0.0; //Prevents error for big change in value after reboot
                __mpu.Reset();
                __mpu.InitHW();
                __mpu._ker.retVal = (int32_t)__mpu.InitSW();
            }
        }
        break;
        /*
         * Soft reboot of MPU -> only reset status in event logger
         * args[] = rebootCode(0x17)
         * retVal one of MPU_* error codes
         */
    case MPU_T_SOFT_REBOOT:
        {
            if (__mpu._ker.args[0] == 0x17)
            {
#ifdef __HAL_USE_EVENTLOG__
                EventLog::SoftReboot(MPU_UID);
#endif  /* __HAL_USE_EVENTLOG__ */
            }
        }
        break;
    default:
        break;
    }

    //  Report outcome to event logger
#ifdef __HAL_USE_EVENTLOG__
    if (__mpu._ker.retVal == MPU_SUCCESS)
        EMIT_EV(__mpu._ker.serviceID, EVENT_OK);
    else
        EMIT_EV(__mpu._ker.serviceID, EVENT_ERROR);
#endif  /* __HAL_USE_EVENTLOG__ */
}
#endif

/*******************************************************************************
 *******************************************************************************
 *********              MPU9250 class member functions                 *********
 *******************************************************************************
 ******************************************************************************/

///-----------------------------------------------------------------------------
///         Functions for returning static instance                     [PUBLIC]
///-----------------------------------------------------------------------------

/**
 * Return reference to a singleton
 * @return reference to an internal static instance
 */
MPU9250& MPU9250::GetI()
{
    static MPU9250 singletonInstance;
    return singletonInstance;
}

/**
 * Return pointer to a singleton
 * @return pointer to a internal static instance
 */
MPU9250* MPU9250::GetP()
{
    return &(MPU9250::GetI());
}

///-----------------------------------------------------------------------------
///         Public functions used for configuring MPU9250               [PUBLIC]
///-----------------------------------------------------------------------------

/**
 * Initialize hardware used by MPU9250
 * Initializes I2C bus for communication with MPU (SDA - PN4, SCL - PN5), bus
 * frequency 1MHz, connection timeout: 100ms. Initializes pin(PA5)
 * to be toggled by MPU9250 when it has data available for reading (PA5 is
 * push-pull pin with weak pull down and 10mA strength).
 * @return One of MPU_* error codes
 */
int8_t MPU9250::InitHW()
{
    HAL_MPU_Init();
    HAL_MPU_PowerSwitch(true);

    //  Emit event before initializing module
#ifdef __HAL_USE_EVENTLOG__
    EMIT_EV(-1, EVENT_STARTUP);
#endif  /* __HAL_USE_EVENTLOG__ */

#if defined(__USE_TASK_SCHEDULER__)
    //  Register module services with task scheduler
    _ker.callBackFunc = _MPU_KernelCallback;
    TS_RegCallback(&_ker, MPU_UID);
#endif

    return MPU_SUCCESS;
}

/**
 * Initialize MPU sensor, load DMP firmware and configure DMP output. Prior to
 * any software initialization, this function power-cycles the board
 * @return One of MPU_* error codes
 */
int8_t MPU9250::InitSW()
{
    int result;

    //  Power cycle MPU chip on every SW initialization
    HAL_MPU_PowerSwitch(false);
    HAL_DelayUS(20000);
    HAL_MPU_PowerSwitch(true);
    HAL_DelayUS(30000);

    //  Emit event before initializing module
#ifdef __HAL_USE_EVENTLOG__
    EMIT_EV(-1, EVENT_STARTUP);
#endif  /* __HAL_USE_EVENTLOG__ */

    mpu_init(&int_param);

    //  Get/set hardware configuration. Start gyro.
    // Wake up all sensors.
    mpu_set_sensors(INV_XYZ_GYRO | INV_XYZ_ACCEL | INV_XYZ_COMPASS);
    // Push accel and quaternion data into the FIFO.
    mpu_configure_fifo(INV_XYZ_ACCEL);
    mpu_set_sample_rate(50);

    // Initialize HAL state variables.
    memset(&hal, 0, sizeof(hal));
#ifdef __DEBUG_SESSION__
    DEBUG_WRITE("Trying to load firmware\n");
#endif
    result = 7; //  Try loading firmware max 7 times
    while(result--)
        if (dmp_load_motion_driver_firmware() == 0)
            break;
#ifdef __DEBUG_SESSION__
        else
            DEBUG_WRITE("%d,  ", result);
#endif

    if (result <= 0)    //  If loading failed 7 times hang here, DMP not usable
    {
#ifdef __DEBUG_SESSION__
        DEBUG_WRITE("   >failed\n");
#endif
        //  Emit error event if using event log, instead of hanging
#ifdef __HAL_USE_EVENTLOG__
    EMIT_EV(-1, EVENT_ERROR);
    return MPU_ERROR;
#else
    while(1);
#endif  /* __HAL_USE_EVENTLOG__ */
    }

#ifdef __DEBUG_SESSION__
    DEBUG_WRITE(" >Firmware loaded\n");
    DEBUG_WRITE(" >Updating DMP features...");
#endif
    dmp_set_orientation(inv_orientation_matrix_to_scalar(gyro_orientation));

    hal.dmp_features = DMP_FEATURE_6X_LP_QUAT | DMP_FEATURE_TAP |
        DMP_FEATURE_ANDROID_ORIENT | DMP_FEATURE_SEND_RAW_ACCEL | DMP_FEATURE_SEND_CAL_GYRO |
        DMP_FEATURE_GYRO_CAL;
    dmp_enable_feature(hal.dmp_features);

    dmp_set_fifo_rate(20);//50
    mpu_set_dmp_state(1);
    hal.dmp_on = 1;
#ifdef __DEBUG_SESSION__
    DEBUG_WRITE("done\n");
#endif

    //  Update status of initialization process
#ifdef __HAL_USE_EVENTLOG__
    EMIT_EV(-1, EVENT_INITIALIZED);
#endif  /* __HAL_USE_EVENTLOG__ */

    return MPU_SUCCESS;
}

/**
 * Trigger software reset of the MPU module by writing into corresponding
 * register. Wait for 50ms afterwards for sensor to start up.
 * @return One of MPU_* error codes
 */
int8_t MPU9250::Reset()
{
    HAL_MPU_WriteByte(MPU9250_ADDRESS, PWR_MGMT_1, 1 << 7);
    HAL_DelayUS(50000);

#ifdef __HAL_USE_EVENTLOG__
    EMIT_EV(-1, EVENT_UNINITIALIZED);
#endif  /* __HAL_USE_EVENTLOG__ */

    return MPU_SUCCESS;
}

/**
 * Control power supply of the MPU9250
 * Enable or disable power supply of the MPU9250 using external MOSFET
 * @param en Power state
 * @return One of MPU_* error codes
 */
int8_t MPU9250::Enabled(bool en)
{
    HAL_MPU_PowerSwitch(en);

#ifdef __HAL_USE_EVENTLOG__
    if (en)
        EMIT_EV(-1, EVENT_UNINITIALIZED);
    else
        EMIT_EV(-1, EVENT_STARTUP);
#endif  /* __HAL_USE_EVENTLOG__ */

    return MPU_SUCCESS;
}

/**
 * Check if new sensor data has been received
 * @return true if new sensor data is available
 *        false otherwise
 */
bool MPU9250::IsDataReady()
{
    return HAL_MPU_DataAvail();
}

/**
 * Get ID from MPU, should always return 0x71
 * @return ID value stored in MPU's register
 */
uint8_t MPU9250::GetID()
{
    uint8_t ID;
    ID = HAL_MPU_ReadByte(MPU9250_ADDRESS, WHO_AM_I_MPU9250);

    return ID;
}

/**
 * Trigger reading data from MPU9250
 * Read data from MPU9250s' FIFO and extract quaternions, acceleration, gravity
 * vector & roll-pitch-yaw
 * @return One of MPU_* error codes
 */
int8_t MPU9250::ReadSensorData()
{
    int8_t retVal = MPU_ERROR;
    short gyro[3], accel[3], sensors;
    unsigned char more = 1;
    long quat[4];
    unsigned long sensor_timestamp;
    int cnt = 0;


     //  Make sure the fifo is empty before leaving this loop, in
     //  order to prevent fifo overflow on consecutive sensor reading
     while (cnt < 100)   //Read max 100 packets, if there's more we
                         //   have a problem
     {
         /* This function gets new data from the FIFO when the DMP is in
           * use. The FIFO can contain any combination of gyro, accel,
           * quaternion, and gesture data. The sensors parameter tells the
           * caller which data fields were actually populated with new data.
           * For example, if sensors == (INV_XYZ_GYRO | INV_WXYZ_QUAT), then
           * the FIFO isn't being filled with accel data.
           * The driver parses the gesture data to determine if a gesture
           * event has occurred; on an event, the application will be notified
           * via a callback (assuming that a callback function was properly
           * registered). The more parameter is non-zero if there are
           * leftover packets in the FIFO.
           */
         retVal = dmp_read_fifo(gyro, accel, quat, &sensor_timestamp, &sensors, &more);
         cnt++;
#ifdef __DEBUG_SESSION__
         if (retVal == (-2))
             DEBUG_WRITE("READ_FIFO returned: %d \n", retVal);
#endif  /* __DEBUG_SESSION__ */

         if (sensors == 0)   //No data available
         {
             retVal = MPU_SUCCESS;
             break;
         }

         //  If reading fifo returned error, move to next packet
         if (retVal)
             continue;

         //  If there was no error, extract orientation data
         Quaternion qt;
         qt.x = (float)quat[0]/QUAT_SENS;
         qt.y = (float)quat[1]/QUAT_SENS;
         qt.z = (float)quat[2]/QUAT_SENS;
         qt.w = (float)quat[3]/QUAT_SENS;

         VectorFloat v;
         dmp_GetGravity(&v, &qt);

         dmp_GetYawPitchRoll((float*)(_ypr), &qt, &v);

         _quat[0] = qt.x;
         _quat[1] = qt.y;
         _quat[2] = qt.z;
         _quat[3] = qt.w;

         //  Copy to MPU class
         _gv[0] = v.x;
         _gv[1] = v.y;
         _gv[2] = v.z;

         _acc[0] = (float)accel[0]/32767.0;
         _acc[1] = (float)accel[1]/32767.0;
         _acc[2] = (float)accel[2]/32767.0;
     }

     return retVal;
}

/**
 * Copy orientation from internal buffer to user-provided one
 * @param RPY pointer to float buffer of size 3 to hold roll-pitch-yaw
 * @param inDeg if true RPY returned in degrees, if false in radians
 * @return One of MPU_* error codes
 */
int8_t MPU9250::RPY(float* RPY, bool inDeg)
{
    for (uint8_t i = 0; i < 3; i++)
        if (inDeg)
            RPY[i] = _ypr[i]*180.0/PI_CONST;
        else
            RPY[i] = _ypr[i];

    return MPU_SUCCESS;
}

/**
 * Copy acceleration from internal buffer to user-provided one
 * @param acc Pointer a float array of min. size 3 to store 3-axis acceleration
 *        data
 * @return One of MPU_* error codes
 */
int8_t MPU9250::Acceleration(float *acc)
{
    memcpy((void*)acc, (void*)_acc, sizeof(float)*3);

    return MPU_SUCCESS;
}

/**
 * Copy angular rotation from internal buffer to user-provided one
 * @param gyro Pointer a float array of min. size 3 to store 3-axis rotation
 *        data
 * @return One of MPU_* error codes
 */
int8_t MPU9250::Gyroscope(float *gyro)
{
    //  Not available
    gyro[0] = gyro[1] = gyro[2] = 0.0f;

    return MPU_SUCCESS;
}

/**
 * Copy mag. field strength from internal buffer to user-provided one
 * @param mag Pointer a float array of min. size 3 to store 3-axis mag. field
 *        strength data
 * @return One of MPU_* error codes
 */
int8_t MPU9250::Magnetometer(float *mag)
{
    //  Not available
    mag[0] = mag[1] = mag[2] = 0.0f;

    return MPU_SUCCESS;
}

///-----------------------------------------------------------------------------
///                      Class constructor & destructor              [PROTECTED]
///-----------------------------------------------------------------------------

MPU9250::MPU9250() :  dT(0), _magEn(true)
{
#ifdef __HAL_USE_EVENTLOG__
    EMIT_EV(-1, EVENT_UNINITIALIZED);
#endif  /* __HAL_USE_EVENTLOG__ */

    //  Initialize arrays
    memset((void*)_ypr, 0, 3);
    memset((void*)_acc, 0, 3);
    memset((void*)_gyro, 0, 3);
    memset((void*)_mag, 0, 3);
}

MPU9250::~MPU9250()
{}

#endif  /* __HAL_USE_MPU9250_DMP__ */
