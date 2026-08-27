#include "CellularPredictive.h"

//======= Constructor Destructor
CellularPredictive::CellularPredictive(QObject *parent) : QObject(parent) , _lastRSRP(0.0f) , _lastTimestamp(0)
{
    _cellular.RSRP = 999.9f;
    _cellular.d_RSRP = 999.9f;
    _cellular.SNIR = 999.9f;
}
CellularPredictive::~CellularPredictive() {}

//======= CPA Functions
void CellularPredictive::processCellularInfo(double RSRP, double SNIR)
{
    if(RSRP > 100.0 && SNIR > 100.0)
    {
        emit alphaValuesCalculated(1.0, 1.0, 1.0);
        return;
    }
    else
    {
        qint64 currentTimestamp = QDateTime::currentMSecsSinceEpoch();

        float deltaTime = 1.0f;
        if (_lastTimestamp > 0 && currentTimestamp > _lastTimestamp)
        {
            deltaTime = (currentTimestamp - _lastTimestamp) / 1000.0f;
            // Additional protection: ensure deltaTime is reasonable (0.001s to 60s)
            deltaTime = constrain_val(deltaTime, 0.001f, 60.0f);
        }

        _cellular.RSRP = RSRP;
        _cellular.d_RSRP = (RSRP - _lastRSRP) / deltaTime;
        _cellular.SNIR = SNIR;

        qDebug() << "CellularPredictive::processCellularInfo - Input:";
        qDebug() << "  - RSRP:" << RSRP << "dBm";
        qDebug() << "  - d_RSRP:" << _cellular.d_RSRP << "dBm/s (dt:" << deltaTime << "s)";
        qDebug() << "  - SNIR:" << SNIR << "dB";

        _lastRSRP = RSRP;
        _lastTimestamp = currentTimestamp;

        // Calculate fuzzy outputs
        float alpha, gainUp, gainDown;
        calculate_fuzzy_outputs(_cellular, &alpha, &gainUp, &gainDown);

        qDebug() << "CellularPredictive::processCellularInfo - Calculated:";
        qDebug() << "  - Alpha:" << alpha << "GainUp:" << gainUp << "GainDown:" << gainDown;

        emit alphaValuesCalculated(static_cast<double>(alpha), static_cast<double>(gainUp), static_cast<double>(gainDown));
    }
}

//======= CPA Helper Functions
float CellularPredictive::interpolate_logic(const float table[LUT_SIZE][LUT_SIZE][LUT_SIZE], cellularData cellular)
{
    float r = constrain_val(cellular.RSRP, RSRP_MIN, RSRP_MAX);
    float d = constrain_val(cellular.d_RSRP, DRSRP_MIN, DRSRP_MAX);
    float s = constrain_val(cellular.SNIR, SNIR_MIN, SNIR_MAX);
    float idx_r = (r - RSRP_MIN) / (RSRP_MAX - RSRP_MIN) * (LUT_SIZE - 1);
    float idx_d = (d - DRSRP_MIN) / (DRSRP_MAX - DRSRP_MIN) * (LUT_SIZE - 1);
    float idx_s = (s - SNIR_MIN) / (SNIR_MAX - SNIR_MIN) * (LUT_SIZE - 1);

    int x0 = static_cast<int>(idx_r);
    int y0 = static_cast<int>(idx_d);
    int z0 = static_cast<int>(idx_s);

    if (x0 >= LUT_SIZE - 1) x0 = LUT_SIZE - 2;
    if (y0 >= LUT_SIZE - 1) y0 = LUT_SIZE - 2;
    if (z0 >= LUT_SIZE - 1) z0 = LUT_SIZE - 2;

    int x1 = x0 + 1;
    int y1 = y0 + 1;
    int z1 = z0 + 1;

    float xd = idx_r - x0;
    float yd = idx_d - y0;
    float zd = idx_s - z0;

    float c000 = table[x0][y0][z0];
    float c001 = table[x0][y0][z1];
    float c010 = table[x0][y1][z0];
    float c011 = table[x0][y1][z1];
    float c100 = table[x1][y0][z0];
    float c101 = table[x1][y0][z1];
    float c110 = table[x1][y1][z0];
    float c111 = table[x1][y1][z1];

    float c00 = c000 * (1.0f - xd) + c100 * xd;
    float c01 = c001 * (1.0f - xd) + c101 * xd;
    float c10 = c010 * (1.0f - xd) + c110 * xd;
    float c11 = c011 * (1.0f - xd) + c111 * xd;

    float c0 = c00 * (1.0f - yd) + c10 * yd;
    float c1 = c01 * (1.0f - yd) + c11 * yd;

    return c0 * (1.0 - zd) + c1 * zd;
}
void CellularPredictive::calculate_fuzzy_outputs(cellularData cellular, float* out_alpha, float* out_up, float* out_down)
{
    // *out_alpha = interpolate_logic(lut_alpha, cellular);
    *out_alpha = 1.0;
    *out_up    = interpolate_logic(lut_up, cellular);
    *out_down  = interpolate_logic(lut_down, cellular);
}
