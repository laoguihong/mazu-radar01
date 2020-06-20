#ifndef __RADAR01_TLV_H__
#define __RADAR01_TLV_H__

#include "vender/dpif_pointcloud.h"

#define MAXIMUM_OBJS 32

struct radar01_message_data_t {
    /* MMWDEMO_OUTPUT_MSG_DETECTED_POINTS = 1 */
    // sizeof(DPIF_PointCloudCartesian) * result->numObjOut
    DPIF_PointCloudCartesian *points;

    /* MMWDEMO_OUTPUT_MSG_RANGE_PROFILE = 2 */
    // sizeof(uint16_t) * subFrameCfg->numRangeBins;
    uint16_t *range_prof;

    /* MMWDEMO_OUTPUT_MSG_NOISE_PROFILE = 3*/
    // sizeof(uint16_t) * subFrameCfg->numRangeBins;
    uint16_t *noise_prof;

    /* MMWDEMO_OUTPUT_MSG_AZIMUT_STATIC_HEAT_MAP = 4*/
    // result->azimuthStaticHeatMapSize * sizeof(cmplx16ImRe_t)
    uint16_t *azimut_static_heat_map;

    /* MMWDEMO_OUTPUT_MSG_RANGE_DOPPLER_HEAT_MAP = 5*/
    // subFrameCfg->numRangeBins * subFrameCfg->numDopplerBins *
    // sizeof(uint16_t)
    uint16_t *range_doppler_heat_map;

    /* MMWDEMO_OUTPUT_MSG_STATS = 6*/
    MmwDemo_output_message_stats msg_stats;

    /* MMWDEMO_OUTPUT_MSG_DETECTED_POINTS_SIDE_INFO = 7 */
    // sizeof(DPIF_PointCloudSideInfo) * result->numObjOut
    DPIF_PointCloudSideInfo *points_side_info;

    /* MMWDEMO_OUTPUT_MSG_AZIMUT_ELEVATION_STATIC_HEAT_MAP = 8 */
    // Not support yet
    /* MMWDEMO_OUTPUT_MSG_TEMPERATURE_STATS = 9*/
    MmwDemo_temperatureStats temp_stats;
}

#endif  //  __RADAR01_TLV_H__