#include <rclcpp/rclcpp.hpp>

#include <sensor_msgs/msg/image.hpp>
#include <visualization_msgs/msg/marker.hpp>

#include <onnx/onnx_tracker.h>
#include <onnx/yolo_processor.h>

#include <algorithm>
#include <numeric>
#include <functional>

namespace yolo 
{
    const int ROW_COUNT = 13;
    const int COL_COUNT = 13;
    const int CHANNEL_COUNT = 125;
    const int BOXES_PER_CELL = 5;
    const int BOX_INFO_FEATURE_COUNT = 5;
    const int CLASS_COUNT = 20;
    const float CELL_WIDTH = 32;
    const float CELL_HEIGHT = 32;

    static const std::string labels[CLASS_COUNT] =
    {
        "aeroplane", "bicycle", "bird", "boat", "bottle",
        "bus", "car", "cat", "chair", "cow",
        "diningtable", "dog", "horse", "motorbike", "person",
        "pottedplant", "sheep", "sofa", "train", "tvmonitor"
    };

    const std::string kDefaultLabel = "person";

    YoloProcessor::YoloProcessor()
    {
        _normalize = false;
    }

    bool YoloProcessor::init(rclcpp::Node::SharedPtr& node) 
    {
        OnnxProcessor::init(node);

        _channelCount = CHANNEL_COUNT;
        _rowCount = ROW_COUNT;
        _colCount = COL_COUNT;
        _outName = {"grid"}; 
        _inName = {"image"};
        _node->get_parameter_or("label", _label, kDefaultLabel);

        return true;
    }

    void YoloProcessor::ProcessOutput(std::vector<float> output, cv::Mat& image)
    {
        if (_fake)
        {
            return;
        }
        
        auto boxes = GetRecognizedObjects(output, _confidence);

        // If we found a person, send a message
        int count = 0;
        for (std::vector<YoloBox>::iterator it = boxes.begin(); it != boxes.end(); ++it)
        {
            if (it->label == _label)
            {
                visualization_msgs::msg::Marker marker;
                marker.header.frame_id = _linkName; 
                marker.header.stamp = rclcpp::Time();
                marker.ns = "onnx_object_detection";
                marker.id = count++;
                marker.type = visualization_msgs::msg::Marker::ARROW;
                marker.action = visualization_msgs::msg::Marker::ADD;

                marker.pose.position.x = it->x + it->width / 2;
                marker.pose.position.y = it->y + it->height / 2;
                marker.pose.position.z = 0;
                marker.pose.orientation.x = 0.0;
                marker.pose.orientation.y = 0.0;
                marker.pose.orientation.z = 0.0;
                marker.pose.orientation.w = 1.0;

                marker.scale.x = 1;
                marker.scale.y = 0.1;
                marker.scale.z = 0.1;
                marker.color.a = 1.0;
                marker.color.r = 0.0;
                marker.color.g = 0.0;
                marker.color.b = 1.0;

                publisher_->publish(marker);
                
                if (_debug)
                {
                    RCLCPP_INFO(_node->get_logger(), "matched label: %s", _label.c_str());
                    // Draw a bounding box on the CV image
                    cv::Scalar color(255, 255, 0);
                    cv::Rect box;
                    box.x = std::max<int>((int)it->x, 0);
                    box.y = std::max<int>((int)it->y, 0);
                    box.height = std::min<int>(image.rows - box.y, (int)it->height);
                    box.width = std::min<int>(image.cols - box.x, (int)it->width);
                    cv::rectangle(image, box, color, 2, 8, 0);
                }

            }
        }

        // Always publish the resized image
        auto msg = cv_bridge::CvImage(std_msgs::msg::Header(), "bgr8", image).toImageMsg();
        msg->header.frame_id = _linkName; 
        image_pub_->publish(*msg);

    }

    std::vector<YoloBox> YoloProcessor::GetRecognizedObjects(std::vector<float> modelOutputs, float threshold)
    {
        static float anchors[] =
        {
            1.08f, 1.19f, 3.42f, 4.41f, 6.63f, 11.38f, 9.42f, 5.11f, 16.62f, 10.52f
        };
        static int featuresPerBox = BOX_INFO_FEATURE_COUNT + CLASS_COUNT;
        static int stride = featuresPerBox * BOXES_PER_CELL;

        std::vector<YoloBox> boxes;

        for (int cy = 0; cy < ROW_COUNT; cy++)
        {
            for (int cx = 0; cx < COL_COUNT; cx++)
            {
                for (int b = 0; b < BOXES_PER_CELL; b++)
                {
                    int channel = (b * (CLASS_COUNT + BOX_INFO_FEATURE_COUNT));
                    float tx = modelOutputs[GetOffset(cx, cy, channel)];
                    float ty = modelOutputs[GetOffset(cx, cy, channel + 1)];
                    float tw = modelOutputs[GetOffset(cx, cy, channel + 2)];
                    float th = modelOutputs[GetOffset(cx, cy, channel + 3)];
                    float tc = modelOutputs[GetOffset(cx, cy, channel + 4)];

                    float x = ((float)cx + Sigmoid(tx)) * CELL_WIDTH;
                    float y = ((float)cy + Sigmoid(ty)) * CELL_HEIGHT;
                    float width = (float)exp(tw) * CELL_WIDTH * anchors[b * 2];
                    float height = (float)exp(th) * CELL_HEIGHT * anchors[b * 2 + 1];

                    float confidence = Sigmoid(tc);
                    if (confidence < threshold)
                        continue;

                    std::vector<float> classes(CLASS_COUNT);
                    float classOffset = channel + BOX_INFO_FEATURE_COUNT;

                    for (int i = 0; i < CLASS_COUNT; i++)
                        classes[i] = modelOutputs[GetOffset(cx, cy, i + classOffset)];

                    Softmax(classes);

                    // Get the index of the top score and its value
                    auto iter = std::max_element(classes.begin(), classes.end());
                    float topScore = (*iter) * confidence;
                    int topClass = std::distance(classes.begin(), iter);

                    if (topScore < threshold)
                        continue;

                    YoloBox top_box = {
                        labels[topClass],
                        (x - width / 2),
                        (y - height / 2),
                        width,
                        height,
                        topScore
                    };
                    boxes.push_back(top_box);
                }
            }
        }

        return boxes;
    }

    int YoloProcessor::GetOffset(int x, int y, int channel)
    {
        // YOLO outputs a tensor that has a shape of 125x13x13, which 
        // WinML flattens into a 1D array.  To access a specific channel 
        // for a given (x,y) cell position, we need to calculate an offset
        // into the array
        static int channelStride = ROW_COUNT * COL_COUNT;
        return (channel * channelStride) + (y * COL_COUNT) + x;
    }

    float YoloProcessor::Sigmoid(float value)
    {
        float k = (float)std::exp(value);
        return k / (1.0f + k);
    }

    void YoloProcessor::Softmax(std::vector<float> &values)
    {
        float max_val{ *std::max_element(values.begin(), values.end()) };
        std::transform(values.begin(), values.end(), values.begin(),
            [&](float x) { return std::exp(x - max_val); });

        float exptot = std::accumulate(values.begin(), values.end(), 0.0);
        std::transform(values.begin(), values.end(), values.begin(),
            [&](float x) { return (float)(x / exptot); });
    }
}