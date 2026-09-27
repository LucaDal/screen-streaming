#pragma once
#include <QVideoFrame>
typedef struct _GstSample GstSample;
QVideoFrame videoFrameFromGstSample(GstSample* sample);
