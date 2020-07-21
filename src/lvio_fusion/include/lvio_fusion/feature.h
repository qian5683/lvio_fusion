#ifndef lvio_fusion_FEATURE_H
#define lvio_fusion_FEATURE_H

#include "lvio_fusion/common.h"

namespace lvio_fusion
{

class Frame;

class MapPoint;

class Feature
{
public:
    typedef std::shared_ptr<Feature> Ptr;

    Feature() {}
    
    static Feature::Ptr Create(std::shared_ptr<Frame> frame, const Vector2d &kp, std::shared_ptr<MapPoint> mappoint)
    {
        Feature::Ptr new_feature(new Feature);
        new_feature->frame = frame;
        new_feature->keypoint = kp;
        new_feature->mappoint = mappoint;
        return new_feature;
    }

    std::weak_ptr<Frame> frame;
    Vector2d keypoint;
    std::weak_ptr<MapPoint> mappoint;
    bool is_on_left_image = true;
};
} // namespace lvio_fusion

#endif // lvio_fusion_FEATURE_H
