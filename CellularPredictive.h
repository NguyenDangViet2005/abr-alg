#ifndef CELLULARPREDICTIVE_H
#define CELLULARPREDICTIVE_H

#include <QDebug>
#include <QObject>
#include <QDateTime>
#include "lib/fuzzyData.h"


struct cellularData { float RSRP; float d_RSRP; float SNIR; };

class CellularPredictive : public QObject
{
    Q_OBJECT
private:
    //======= Member variables for CPA
    float _lastRSRP;
    qint64 _lastTimestamp;
    cellularData _cellular;

    // ======= CPA Helper Functions
    static float constrain_val(float x, float min, float max) { return (x < min) ? min : (x > max) ? max : x; }
    float interpolate_logic(const float table[LUT_SIZE][LUT_SIZE][LUT_SIZE], cellularData cellular);
    void calculate_fuzzy_outputs(cellularData cellular, float* out_alpha, float* out_up, float* out_down);

public:
    //======= Constructor Destructor
    explicit CellularPredictive(QObject *parent = nullptr);
    ~CellularPredictive();

public slots:
              //======= CPA Functions
    void processCellularInfo(double RSRP, double SNIR);   // khớp signal onCellularNetworkInfo(double,double)
signals:
    void alphaValuesCalculated(double alpha, double gainUp, double gainDown);
};

#endif // CELLULARPREDICTIVE_H
