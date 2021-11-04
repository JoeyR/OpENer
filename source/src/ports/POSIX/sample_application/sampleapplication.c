/*******************************************************************************
 * Copyright (c) 2012, Rockwell Automation, Inc.
 * All rights reserved.
 *
 ******************************************************************************/

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "appcontype.h"
#include "cipidentity.h"
#include "cipqos.h"
#include "ciptcpipinterface.h"
#include "nvdata.h"
#include "opener_api.h"
#include "trace.h"
#if defined(OPENER_ETHLINK_CNTRS_ENABLE) && 0 != OPENER_ETHLINK_CNTRS_ENABLE
#include "cipethernetlink.h"
#include "ethlinkcbs.h"
#endif

#include <unistd.h>

#define INTERROLL_DEVICE_INPUT_ASSEMBLY_NUM     101
#define INTERROLL_DEVICE_OUTPUT_ASSEMBLY_NUM    100
#define INTERROLL_DEVICE_CONFIG_ASSEMBLY_NUM      1

// The values below are incorrect and need to updated when we 
// start using them.
#define INTERROLL_DEVICE_HEARTBEAT_INPUT_ONLY_ASSEMBLY_NUM 152   // 0x098
#define INTERROLL_DEVICE_HEARTBEAT_LISTEN_ONLY_ASSEMBLY_NUM 153  // 0x099
#define INTERROLL_DEVICE_EXPLICT_ASSEMBLY_NUM 107                // 0x6B

#define MAX_ZONES 4
#define FIRST_ZONE 0
#define LAST_ZONE 3
#define ZONE_0 0
#define ZONE_1 1
#define ZONE_2 2
#define ZONE_3 3

// Arbitrary package transition time, will likely needed to be tuned over time.
#define PACKAGE_TRANSITION_TIME 600000

/* global variables for demo application (4 assembly data fields)  ************/

typedef struct {
  uint32_t padding;
  uint8_t sensors;
  uint8_t digital_io;
  uint8_t motor_states;
  int8_t motor_speed_1;
  int8_t motor_speed_2;
  int8_t motor_speed_3;
  int8_t motor_speed_4;
  int8_t motor_states_spare;
  uint16_t motor_current_1;
  uint16_t motor_current_2;
  uint16_t motor_current_3;
  uint16_t motor_current_4;
  uint16_t motor_voltage;
  uint16_t logic_voltage;
  int16_t temperature;
  uint32_t uptime;
  uint8_t control_inputs;
  uint8_t decision_byte;
  uint8_t control_outputs;
  uint8_t handshake_signals;
  uint8_t zone_states;
  uint8_t zone_error_1;
  uint8_t zone_error_2;
  uint8_t zone_error_3;
  uint8_t zone_error_4;
  uint8_t spare;
} __attribute__((__packed__)) InterrollInput;

typedef struct {
  uint8_t digital_outputs;
  uint8_t motor_1_speed_pos;
  uint8_t motor_2_speed_pos;
  uint8_t motor_3_speed_pos;
  uint8_t motor_4_speed_pos;
  uint8_t control_inputs_overwrite;
  uint8_t selection;
  uint8_t control_outputs_overwrite;
  uint8_t handshake_signals_overwrite;
  uint8_t global_signals_overwrite;
} InterrollOutput;

InterrollInput g_assembly_data064; /* Input */
InterrollOutput g_assembly_data096;  /* Output */
EipUint8 g_assembly_data097[10];  /* Config */
EipUint16 g_assembly_data06B[10]; /* Explicit */

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
pthread_t g_convey_in_thread = PTHREAD_ONCE_INIT;

pthread_t g_convey_out_thread = PTHREAD_ONCE_INIT;
pthread_mutex_t g_convey_out_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_convey_out_cond = PTHREAD_COND_INITIALIZER;

/* Signal for thread exit, called during session close */
pthread_mutex_t g_exit_thread_mutex = PTHREAD_MUTEX_INITIALIZER;
EipBool8 g_exit_thread = false;

typedef struct {
  EipBool8 occupied;
  EipByte sensor;
  pthread_mutex_t mutex;
} Zone;

pthread_mutex_t g_zone_lock = PTHREAD_MUTEX_INITIALIZER;
Zone g_zones[MAX_ZONES];

void ZoneConveyIn(Zone* cur_zone);
void ZoneConveyOut(Zone* cur_zone);

void PrintZoneState() {
  for (int i = 0; i < MAX_ZONES; i++) {
    OPENER_TRACE_INFO("ZONE : %d\n", i + 1);
    pthread_mutex_lock(&g_zones[i].mutex);
    OPENER_TRACE_INFO("OCCUPIED : %d\n", g_zones[i].occupied);
    OPENER_TRACE_INFO("SENSOR : %d\n", g_zones[i].sensor);
    pthread_mutex_unlock(&g_zones[i].mutex);
  }
}

EipBool8 ShouldExit() {

    pthread_mutex_lock(&g_exit_thread_mutex);
    EipBool8 should_exit = g_exit_thread;
    pthread_mutex_unlock(&g_exit_thread_mutex);

    return should_exit;
}

/* local functions */
void* ConveyOutThread(void* arg) {

  pthread_detach(pthread_self());

  while (true) {

    pthread_mutex_unlock(&g_zone_lock);

    pthread_mutex_lock(&g_convey_out_lock);
    OPENER_TRACE_INFO("Device waiting for convey out signal\n");
    pthread_cond_wait(&g_convey_out_cond, &g_convey_out_lock);
    OPENER_TRACE_INFO("Device recieved convey out signal\n");
    pthread_mutex_unlock(&g_convey_out_lock);

    // Check if the thread should exit because we got 
    // a device reset.
    if (ShouldExit() == true){
      OPENER_TRACE_INFO("Convey out thread recieved exit");
      pthread_exit(NULL);
    }

    // Making changes to zones.
    pthread_mutex_lock(&g_zone_lock);

    pthread_mutex_lock(&g_zones[LAST_ZONE].mutex);
    EipBool8 is_last_zone_occupied = g_zones[LAST_ZONE].occupied;
    pthread_mutex_unlock(&g_zones[LAST_ZONE].mutex);

    if (is_last_zone_occupied == false) {
      OPENER_TRACE_ERR("The last zone is empty, nothing to convey out");
      continue;
    }

    int cur_zone = LAST_ZONE;
    int prev_zone = cur_zone - 1;
    ZoneConveyOut(&g_zones[cur_zone]);

    do {  

      pthread_mutex_lock(&g_zones[prev_zone].mutex);
      EipBool8 is_occupied = g_zones[prev_zone].occupied;
      pthread_mutex_unlock(&g_zones[prev_zone].mutex);

      if (is_occupied) {
        ZoneConveyOut(&g_zones[prev_zone]);

        ZoneConveyIn(&g_zones[cur_zone]);

        cur_zone = prev_zone;
        prev_zone = cur_zone - 1;
      } else {
        break;
      }

    } while (prev_zone >= FIRST_ZONE);
  }
}

void* ConveyInThread(void* arg) {

  pthread_detach(pthread_self());

  while (true) {

    pthread_mutex_unlock(&g_zone_lock);

    pthread_mutex_lock(&g_lock);
    OPENER_TRACE_INFO("Device waiting for convey in signal\n");
    pthread_cond_wait(&g_cond, &g_lock);
    OPENER_TRACE_INFO("Device recieved convey in signal\n");
    pthread_mutex_unlock(&g_lock);

    pthread_mutex_lock(&g_zone_lock);

    // Check during device reset to exit current thread.
    if (ShouldExit() == true){
      OPENER_TRACE_INFO("Convey in thread recieved exit");
      pthread_exit(NULL);
    }

    OPENER_TRACE_INFO("The worker thread is running\n");
    if (g_zones[FIRST_ZONE].occupied == true) {
      OPENER_TRACE_ERR("The Zones are all occupied, package will fall off");
      continue;
    }

    int cur_zone = 0;
    int next_zone = cur_zone + 1;
    while (cur_zone < MAX_ZONES) {

      if (cur_zone != FIRST_ZONE) {
        ZoneConveyOut(&g_zones[cur_zone - 1]);
      }

      ZoneConveyIn(&g_zones[cur_zone]);

      pthread_mutex_lock(&g_zones[next_zone].mutex);
      EipBool8 is_occupied = g_zones[next_zone].occupied;
      pthread_mutex_unlock(&g_zones[next_zone].mutex);

      // Convey into next zone automicatically, accumulator mode
      if (next_zone < MAX_ZONES && is_occupied == false) {
        cur_zone = next_zone;
        next_zone = next_zone + 1;
      } 
      else {
        // No more work to do.
        break;
      }
    }  
  }

  pthread_exit(NULL);

}

EipStatus CreateInterrollSimThread() {

  pthread_create(&g_convey_in_thread, NULL, &ConveyInThread, NULL);

  pthread_create(&g_convey_out_thread, NULL, &ConveyOutThread, NULL);

  return kEipStatusOk;
}

void InitializeInterrollZones() {
  for (int i = 0; i < MAX_ZONES; i++) {
    g_zones[i].occupied = false;
    g_zones[i].sensor = 0;
    pthread_mutex_init(&g_zones[i].mutex, NULL);
  }
}

void ZoneConveyIn(Zone* cur_zone) {
  if (cur_zone == NULL) {
    OPENER_TRACE_INFO("curZone is null !!");
    return;
  }

  if (cur_zone->occupied == true) {
    OPENER_TRACE_ERR("CurrentZone is occupied can not convey in!!");
    return;
  }

  pthread_mutex_lock(&cur_zone->mutex);
  cur_zone->sensor = 1;
  pthread_mutex_unlock(&cur_zone->mutex);

  usleep(PACKAGE_TRANSITION_TIME);

  pthread_mutex_lock(&cur_zone->mutex);
  cur_zone->occupied = true;
  pthread_mutex_unlock(&cur_zone->mutex);
}

void ZoneConveyOut(Zone* cur_zone) {
  if (cur_zone == NULL) {
    OPENER_TRACE_ERR("Current zone is null !!");
    return;
  }

  if (cur_zone->occupied == false) {
    OPENER_TRACE_ERR("Current zone is not occupied can not convey out!!");
    return;
  }

  usleep(PACKAGE_TRANSITION_TIME);

  pthread_mutex_lock(&cur_zone->mutex);
  cur_zone->sensor = 0;
  cur_zone->occupied = false;
  pthread_mutex_unlock(&cur_zone->mutex);
}

/* global functions called by the stack */
EipStatus ApplicationInitialization(void) {
  /* create 3 assembly object instances*/
  /*INPUT*/
  OPENER_TRACE_INFO("Size of input assembly : %d\n", sizeof(g_assembly_data064));
  CreateAssemblyObject(INTERROLL_DEVICE_INPUT_ASSEMBLY_NUM, (EipByte*) &g_assembly_data064,
                       sizeof(g_assembly_data064));

  /*OUTPUT*/
  CreateAssemblyObject(INTERROLL_DEVICE_OUTPUT_ASSEMBLY_NUM, (EipByte*) &g_assembly_data096,
                       sizeof(g_assembly_data096));

  /*CONFIG*/
  CreateAssemblyObject(INTERROLL_DEVICE_CONFIG_ASSEMBLY_NUM, g_assembly_data097,
                       sizeof(g_assembly_data097));

  /*Heart-beat output assembly for Input only connections */
  CreateAssemblyObject(INTERROLL_DEVICE_HEARTBEAT_INPUT_ONLY_ASSEMBLY_NUM, NULL, 0);

  /*Heart-beat output assembly for Listen only connections */
  CreateAssemblyObject(INTERROLL_DEVICE_HEARTBEAT_LISTEN_ONLY_ASSEMBLY_NUM, NULL, 0);

  /* assembly for explicit messaging */
  CreateAssemblyObject(INTERROLL_DEVICE_EXPLICT_ASSEMBLY_NUM, g_assembly_data06B,
                       sizeof(g_assembly_data06B));

  ConfigureExclusiveOwnerConnectionPoint(0, INTERROLL_DEVICE_OUTPUT_ASSEMBLY_NUM,
                                         INTERROLL_DEVICE_INPUT_ASSEMBLY_NUM,
                                         INTERROLL_DEVICE_CONFIG_ASSEMBLY_NUM);
  ConfigureInputOnlyConnectionPoint(
      0, INTERROLL_DEVICE_HEARTBEAT_INPUT_ONLY_ASSEMBLY_NUM,
      INTERROLL_DEVICE_INPUT_ASSEMBLY_NUM, INTERROLL_DEVICE_CONFIG_ASSEMBLY_NUM);
  ConfigureListenOnlyConnectionPoint(
      0, INTERROLL_DEVICE_HEARTBEAT_LISTEN_ONLY_ASSEMBLY_NUM,
      INTERROLL_DEVICE_INPUT_ASSEMBLY_NUM, INTERROLL_DEVICE_CONFIG_ASSEMBLY_NUM);

  /* For NV data support connect callback functions for each object class with
   *  NV data.
   */
  InsertGetSetCallback(GetCipClass(kCipQoSClassCode), NvQosSetCallback,
                       kNvDataFunc);
  InsertGetSetCallback(GetCipClass(kCipTcpIpInterfaceClassCode),
                       NvTcpipSetCallback, kNvDataFunc);

#if defined(OPENER_ETHLINK_CNTRS_ENABLE) && 0 != OPENER_ETHLINK_CNTRS_ENABLE
  /* For the Ethernet Interface & Media Counters connect a PreGetCallback and
   *  a PostGetCallback.
   * The PreGetCallback is used to fetch the counters from the hardware.
   * The PostGetCallback is utilized by the GetAndClear service to clear
   *  the hardware counters after the current data have been transmitted.
   */
  {
    CipClass *p_eth_link_class = GetCipClass(kCipEthernetLinkClassCode);
    InsertGetSetCallback(p_eth_link_class, EthLnkPreGetCallback, kPreGetFunc);
    InsertGetSetCallback(p_eth_link_class, EthLnkPostGetCallback, kPostGetFunc);
    /* Specify the attributes for which the callback should be executed. */
    for (int idx = 0; idx < OPENER_ETHLINK_INSTANCE_CNT; ++idx) {
      CipAttributeStruct *p_eth_link_attr;
      CipInstance *p_eth_link_inst = GetCipInstance(p_eth_link_class, idx + 1);
      OPENER_ASSERT(p_eth_link_inst);

      /* Interface counters attribute */
      p_eth_link_attr = GetCipAttribute(p_eth_link_inst, 4);
      p_eth_link_attr->attribute_flags |= (kPreGetFunc | kPostGetFunc);
      /* Media counters attribute */
      p_eth_link_attr = GetCipAttribute(p_eth_link_inst, 5);
      p_eth_link_attr->attribute_flags |= (kPreGetFunc | kPostGetFunc);
    }
  }
#endif

  InitializeInterrollZones();

  CreateInterrollSimThread();

  return kEipStatusOk;
}

void HandleApplication(void) {
}

void CheckIoConnectionEvent(unsigned int output_assembly_id,
                            unsigned int input_assembly_id,
                            IoConnectionEvent io_connection_event) {
  /* maintain a correct output state according to the connection state*/

  (void)output_assembly_id;  /* suppress compiler warning */
  (void)input_assembly_id;   /* suppress compiler warning */
  (void)io_connection_event; /* suppress compiler warning */
}

EipStatus AfterAssemblyDataReceived(CipInstance *instance) {
  EipStatus status = kEipStatusOk;

  /*handle the data received e.g., update outputs of the device */
  switch (instance->instance_number) {
    case INTERROLL_DEVICE_OUTPUT_ASSEMBLY_NUM:

      // Convey into first zone signal.
      // If convey out signal is high give preference to convey out
      // over convey in.
      if (g_assembly_data096.handshake_signals_overwrite & (1 << 1)) {
        OPENER_TRACE_INFO("Recieved Convey out signal");
        pthread_cond_signal(&g_convey_out_cond);
      }
      if (g_assembly_data096.handshake_signals_overwrite & (1 << 0)) {
        OPENER_TRACE_INFO("Recieved Convey in signal");
        pthread_cond_signal(&g_cond);
      }
      uint8_t result = 0;
      // Update sensor values.
      g_assembly_data064.handshake_signals = 0;
      for (int i = 0; i < MAX_ZONES; i++) {
        pthread_mutex_lock(&g_zones[i].mutex);
        result |= (g_zones[i].sensor == 1) ? (1 << i) : 0;
        pthread_mutex_unlock(&g_zones[i].mutex);
      }
      g_assembly_data064.sensors = result;

      // First zone occupied bit.
      pthread_mutex_lock(&g_zones[FIRST_ZONE].mutex);
      g_assembly_data064.handshake_signals |= (g_zones[FIRST_ZONE].occupied == 1) ? 0 : (1 << 4);
      pthread_mutex_unlock(&g_zones[FIRST_ZONE].mutex);

      // Last zone occupied bit.
      pthread_mutex_lock(&g_zones[LAST_ZONE].mutex);
      g_assembly_data064.handshake_signals |= (g_zones[LAST_ZONE].occupied == 1) ?  (1 << 5) : 0;
      pthread_mutex_unlock(&g_zones[LAST_ZONE].mutex);
      
      PrintZoneState();
      OPENER_TRACE_INFO("Sensor State : %d\n", g_assembly_data064.sensors); 
      OPENER_TRACE_INFO("Hand shake signals : %d\n", g_assembly_data064.handshake_signals); 
      break;
    case INTERROLL_DEVICE_EXPLICT_ASSEMBLY_NUM:
      /* do something interesting with the new data from
       * the explicit set-data-attribute message */
      break;
    case INTERROLL_DEVICE_CONFIG_ASSEMBLY_NUM:
      /* Add here code to handle configuration data and check if it is ok
       * The demo application does not handle config data.
       * However in order to pass the test we accept any data given.
       * EIP_ERROR
       */
      status = kEipStatusOk;
      break;
    default:
      OPENER_TRACE_INFO(
          "Unknown assembly instance ind AfterAssemblyDataReceived");
      break;
  }
  return status;
}

EipBool8 BeforeAssemblyDataSend(CipInstance *pa_pstInstance) {
  /*update data to be sent e.g., read inputs of the device */
  /*In this sample app we mirror the data from out to inputs on data receive
   * therefore we need nothing to do here. Just return true to inform that
   * the data is new.
   */
  if (pa_pstInstance->instance_number == INTERROLL_DEVICE_EXPLICT_ASSEMBLY_NUM) {
    /* do something interesting with the existing data
     * for the explicit get-data-attribute message */
  }
  return true;
}

EipStatus ResetDevice(void) {
  /* add reset code here*/
  OPENER_TRACE_INFO("Resetting device\n");
  // Set thread exit flag.
  pthread_mutex_lock(&g_exit_thread_mutex);
  g_exit_thread = true;
  pthread_mutex_unlock(&g_exit_thread_mutex);

  // Signal all threads to wake up.
  pthread_cond_signal(&g_convey_out_cond);
  pthread_cond_signal(&g_cond);

  pthread_join(&g_convey_out_thread, NULL);
  pthread_join(&g_convey_in_thread, NULL);

  InitializeInterrollZones();

  CloseAllConnections();
  CipQosUpdateUsedSetQosValues();
  return kEipStatusOk;
}

EipStatus ResetDeviceToInitialConfiguration(void) {
  /*rest the parameters */
  g_tcpip.encapsulation_inactivity_timeout = 120;
  CipQosResetAttributesToDefaultValues();
  /*than perform device reset*/
  ResetDevice();
  return kEipStatusOk;
}

void *CipCalloc(size_t number_of_elements, size_t size_of_element) {
  return calloc(number_of_elements, size_of_element);
}

void CipFree(void *data) { free(data); }

void RunIdleChanged(EipUint32 run_idle_value) {
  OPENER_TRACE_INFO("Run/Idle handler triggered\n");
  if ((0x0001 & run_idle_value) == 1) {
    CipIdentitySetExtendedDeviceStatus(kAtLeastOneIoConnectionInRunMode);
  } else {
    CipIdentitySetExtendedDeviceStatus(
        kAtLeastOneIoConnectionEstablishedAllInIdleMode);
  }
  (void)run_idle_value;
}
