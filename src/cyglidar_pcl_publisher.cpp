#include <cyglidar_pcl.h>

const float RADIAN = MATH_PI / HALF_ANGLE;
const float DEGREE = HALF_ANGLE / MATH_PI;

int DATABUFFER_SIZE_2D, DATABUFFER_SIZE_3D, DATASET_SIZE_2D, DATASET_SIZE_3D;

typedef pcl::PointCloud<pcl::PointXYZRGBA> PointXYZRGBA;
pcl::PointCloud<pcl::PointXYZRGBA>::Ptr scan_2D;
pcl::PointCloud<pcl::PointXYZRGBA>::Ptr scan_3D;
sensor_msgs::LaserScan::Ptr scan_laser;
tf::TransformListener *tf_listener;

ros::Publisher pub_2D, pub_3D, pub_scan;

uint8_t *cloudBuffer;
float *cloudSetBuffer;
uint8_t *cloudBuffer_2D;
float *cloudSetBuffer_2D;

uint8_t* bufferPtr_2D;
uint8_t* bufferPtr01;
uint8_t* bufferPtr02;
uint8_t* bufferPtr;

float angleRadian = RADIAN * -RIGHT_ANGLE;
float centerX_3D, centerY_3D, width, height;

uint8_t currentBuffer;

static std::vector<uint32_t> colorArray;
void colorBuffer()
{
    int colors = 0;
    uint8_t r_setup = 254;
    uint8_t g_setup = 0;
    uint8_t b_setup = 0;

    for (int colorSet = 0; colorSet < 2; colorSet++)
    {
        for (int i = 0; i < 3; i++)
        {
            for (int colorCnt = 0; colorCnt < COLOR_MAX; colorCnt++)
            {
                switch (colors)
                {
                    // RED -> YELLOW
                    case 0:
                        g_setup++;
                        break;
                    // YELLOW -> BLUE
                    case 1:
                        r_setup--;
                        g_setup--;
                        b_setup++;
                        break;
                    // BLUE -> RED
                    case 2:
                        r_setup++;
                        b_setup--;
                        break;
                }

                uint32_t rgb_setup = ((uint32_t)r_setup << 16 | (uint32_t)g_setup << 8 | (uint32_t)b_setup);
                colorArray.push_back(rgb_setup);

                if (colorCnt == COLOR_MAX - 1)
                {
                    colors++;
                }
            }
        }
    }

    //ROS_INFO("COLORS STORED (%d)", colorArray.size());
}

bool drawing = false;
float tempX_2D, tempY_2D;
uint8_t cloudScatter_2D(ros::Time start, ros::Time end, double ANGLE_STEP_2D)
{
    if (!drawing)
    {
        drawing = true;
        memcpy(cloudBuffer_2D, &bufferPtr_2D[PAYLOAD_SIZE], sizeof(uint8_t) * DATABUFFER_SIZE_2D);
        
        int distanceCnt_2D = 0;

        for (int dataLength = 0; dataLength < (DATABUFFER_SIZE_2D - PAYLOAD_SIZE) - 1; dataLength+=2)
        {
            uint8_t MSB = cloudBuffer_2D[dataLength];
            uint8_t LSB = cloudBuffer_2D[dataLength + 1];

            float data1 = (float)((MSB << 8) | LSB);

            cloudSetBuffer_2D[distanceCnt_2D++] = data1;
        }

        double scan_duration = (start - end).toSec() * 1e-3;

        scan_laser->header.stamp = start;
        scan_laser->scan_time = scan_duration;
        scan_laser->time_increment = (scan_duration / (float)(DATASET_SIZE_2D - 1));

        float point_angle_var_2D = 0.0;

        for (int idx = 0; idx < DATASET_SIZE_2D; idx++)
        {
            int data_idx = (DATASET_SIZE_2D - 1 - idx);
            float actualDistance = (cloudSetBuffer_2D[data_idx]);
            
            float point_angle_2D = (float)(((HORIZONTAL_ANGLE / 2 * -1) + point_angle_var_2D) * RADIAN);
            point_angle_var_2D += ANGLE_STEP_2D;

            //actualDistance = (cloudSetBuffer_2D[idx]);
            float actualX = (sin(point_angle_2D) * actualDistance);
            float actualY = (cos(point_angle_2D) * actualDistance); // depth

            tempX_2D = actualX;
            tempY_2D = actualY;
            actualX = (tempX_2D * cos(angleRadian)) + (tempY_2D * -sin(angleRadian));
            actualY = (tempX_2D * sin(angleRadian)) + (tempY_2D * cos(angleRadian));

            scan_2D.get()->points[idx].x = actualX * DIVISOR;
            scan_2D.get()->points[idx].y = -actualY * DIVISOR;
            scan_2D.get()->points[idx].z = 0.0;
            
            if (cloudSetBuffer_2D[data_idx] < (float)BASE_DEPTH_2D)
            {
                scan_2D.get()->points[idx].r = 255;
                scan_2D.get()->points[idx].g = 255;
                scan_2D.get()->points[idx].b = 0;
                scan_2D.get()->points[idx].a = 255;
                scan_laser->ranges[idx] = cloudSetBuffer_2D[data_idx] * DIVISOR;
            }
            else
            {
                scan_2D.get()->points[idx].a = 0;
                scan_laser->ranges[idx] = std::numeric_limits<float>::infinity();
            }
        }

        pcl_conversions::toPCL(ros::Time::now(), scan_2D->header.stamp);
        pub_2D.publish(scan_2D);

        pub_scan.publish(scan_laser);

        drawing = false; 
    }
}

uint8_t cloudScatter_3D()
{
    if (!drawing)
    {
        drawing = true;

        switch (currentBuffer)
        {
            case 0x00:
                memcpy(cloudBuffer, &bufferPtr02[PAYLOAD_SIZE], sizeof(uint8_t) * DATABUFFER_SIZE_3D);
                break;
            case 0x01:
                memcpy(cloudBuffer, &bufferPtr01[PAYLOAD_SIZE], sizeof(uint8_t) * DATABUFFER_SIZE_3D);
                break;
        }
        
        int distanceCnt_3D = 0;
        for (int dataLength = 0; dataLength < (DATABUFFER_SIZE_3D - PAYLOAD_SIZE) - 1; dataLength+=3)
        {
            uint8_t FIRST = cloudBuffer[dataLength];
            uint8_t SECOND = cloudBuffer[dataLength + 1];
            uint8_t THIRD = cloudBuffer[dataLength + 2];

            float data1 = (float)((FIRST << 4) | (SECOND >> 4));
            float data2 = (float)(((SECOND & 0xf) << 8) | THIRD);

            cloudSetBuffer[distanceCnt_3D++] = data1;
            cloudSetBuffer[distanceCnt_3D++] = data2;
        }

        float horizontalA_Half = (float)HORIZONTAL_ANGLE / 2;
        float verticalA_Single = (float)VERTICAL_ANGLE / centerX_3D;
        float originalA_H = (atan(centerX_3D / FOCAL_LENGTH) * (HALF_ANGLE / MATH_PI));
        float originalA_V = (atan(centerY_3D / FOCAL_LENGTH) * (HALF_ANGLE / MATH_PI));
        float differenceA_H = (horizontalA_Half / originalA_H);
        float differenceA_V = ((VERTICAL_ANGLE / 2) / originalA_V);
        
        int index_3D = 0;
        for (int yy = 0; yy < height; yy++)
        {
            for (int xx = 0; xx < width; xx++)
            {
                index_3D = (xx + (yy * width));

                float x_3D = abs(xx - centerX_3D);
                float y_3D = abs(yy - centerY_3D);
                
                float tanA1_3D = y_3D / x_3D;
                float h_3D = (y_3D == 0 ? x_3D : (y_3D / sin(atan(tanA1_3D))));

                float tanA2_3D = FOCAL_LENGTH / h_3D;
                float tempD_3D = FOCAL_LENGTH / sin(atan(tanA2_3D));

                float dRatio_3D = (cloudSetBuffer[index_3D] / tempD_3D);
                float actualDistance = (FOCAL_LENGTH * dRatio_3D);

                float actualX = (xx - centerX_3D) * (actualDistance / FOCAL_LENGTH) * differenceA_H;
                float actualY = -(yy - centerY_3D) * (actualDistance / FOCAL_LENGTH) * differenceA_V;
                
                // Rotate the axis of clouds to the right
                float tempX_3D = actualX;
                float tempY_3D = actualDistance;
                
                actualX = (tempX_3D * cos(angleRadian)) + (tempY_3D * -sin(angleRadian));
                actualDistance = (tempX_3D * sin(angleRadian)) + (tempY_3D * cos(angleRadian));
                
                scan_3D.get()->points[index_3D].x = actualX * DIVISOR;
                scan_3D.get()->points[index_3D].y = actualDistance * DIVISOR;
                scan_3D.get()->points[index_3D].z = (actualY + HALF_ANGLE) * DIVISOR;

                // Determine a cloud color based on the distance

                if (cloudSetBuffer[index_3D] < BASE_DEPTH_3D)
                {
                    uint32_t rgb_3D = colorArray[(int)tempY_3D % colorArray.size()];
                    scan_3D.get()->points[index_3D].rgb = *reinterpret_cast<float*>(&rgb_3D);
                    scan_3D.get()->points[index_3D].a = 255;
                }
                else
                {
                    scan_3D.get()->points[index_3D].a = 0;
                }
            }
        }

        pcl_conversions::toPCL(ros::Time::now(), scan_3D->header.stamp);
        pub_3D.publish(scan_3D);

        drawing = false;
    }
}

int VERSION_NUM, FREQUENCY_LEVEL, SENSITIVITY_VALUE, PULSE_CONTROL, PULSE_DURATION;
void running()
{
    ros::NodeHandle nh;
    ros::NodeHandle priv_nh("~");

    std::string port;
    int baud_rate;
    std::string frame_id;

    priv_nh.param("port", port, std::string("/dev/ttyUSB0"));
    priv_nh.param("baud_rate", baud_rate, 3000000);
    priv_nh.param("frame_id", frame_id, std::string("laser_link"));

    priv_nh.param("version", VERSION_NUM, 0);
    priv_nh.param("frequency", FREQUENCY_LEVEL, 0);
    priv_nh.param("sensitivity", SENSITIVITY_VALUE, 20);
    priv_nh.param("pulse_control", PULSE_CONTROL, 0);
    priv_nh.param("duration", PULSE_DURATION, 0);


    ROS_INFO("FREQUENCY: %d (%x) / SENSITIVITY: %d / PULSE CONTROL: %d, DURATION: %d (%x)", \
    FREQUENCY_LEVEL, FREQUENCY_LEVEL, SENSITIVITY_VALUE, \
    PULSE_CONTROL, PULSE_DURATION, PULSE_DURATION);

    colorBuffer();

    pub_scan = nh.advertise<sensor_msgs::LaserScan>("scan_laser", SIZE_MAX);
    pub_2D = nh.advertise<sensor_msgs::PointCloud2>("scan_2D", 1);
    pub_3D = nh.advertise<sensor_msgs::PointCloud2>("scan_3D", 1);
    scan_2D = pcl::PointCloud<pcl::PointXYZRGBA>::Ptr(new pcl::PointCloud<pcl::PointXYZRGBA>);
    scan_3D = pcl::PointCloud<pcl::PointXYZRGBA>::Ptr(new pcl::PointCloud<pcl::PointXYZRGBA>);
    scan_laser = sensor_msgs::LaserScan::Ptr(new sensor_msgs::LaserScan);

    // LaserScan
    scan_laser->header.frame_id = frame_id;
    
    // PointCloud2 for 2D
    scan_2D.get()->is_dense = false;
    scan_2D.get()->header.frame_id = frame_id;

    // PointCloud2 for 3D
    scan_3D.get()->width = 160;
    scan_3D.get()->height = 60;
    scan_3D.get()->is_dense = false;
    scan_3D.get()->header.frame_id = frame_id;

    width = (float)(scan_3D.get()->width);
    height = (float)(scan_3D.get()->height);
    
    centerX_3D = (width / 2);
    centerY_3D = (height / 2);

    currentBuffer = 0x00;
    drawing = false;

    bufferPtr = new uint8_t[SIZE_MAX];
    uint8_t* result;
    uint8_t parser, inProgress = 0x00;
	int bytes_transferred;
	size_t sizePos = 2;

    ros::Time current_time_laser = ros::Time::now();
    ros::Time last_time_laser = ros::Time::now();

    bool buffer_setup_2d = false;
    bool buffer_setup_3d = false;
    
    boost::asio::io_service io;
    try
    {
        cyglidar_pcl_driver::cyglidar_pcl laser(port, baud_rate, io);
        laser.packet_sensitivity(SENSITIVITY_VALUE);
        ros::Duration(1.0).sleep();
        laser.packet_run(VERSION_NUM);
        ros::Duration(1.0).sleep();
        laser.packet_frequency(FREQUENCY_LEVEL);
        ros::Duration(1.0).sleep();
        laser.packet_pulse(VERSION_NUM, PULSE_CONTROL, PULSE_DURATION);

        int DATA_1 = 4, DATA_2 = 3;
        double ANGLE_STEP_2D, ANGLE_POINT_2D;

        while (ros::ok())
        {
            if (inProgress == 0x00)
            {
				inProgress = 0x01;

                result = laser.poll(VERSION_NUM);               
                bytes_transferred = (int)(result[SIZE_MAX] << 8 | result[SIZE_MAX + 1]);

                if (bytes_transferred > 0)
                {
                    for (int i = 0; i < bytes_transferred; i++)
                    {
                        parser = CygParser(bufferPtr, result[i]);

                        switch (parser)
                        {
                            case 0x01:
                                //ROS_ERROR("DATA >> %x, %x, %x, %x, %x, %x",\
                                bufferPtr[0], bufferPtr[1], bufferPtr[2], bufferPtr[3], bufferPtr[4], bufferPtr[5]);
                                switch (bufferPtr[5])
                                {
                                    case 0x01: // 2D
                                        if (!buffer_setup_2d)
                                        {
                                            DATABUFFER_SIZE_2D = (int)(bufferPtr[DATA_1] << 8 | bufferPtr[DATA_2]) + PAYLOAD_SIZE;
                                            DATASET_SIZE_2D = (int)((float)(DATABUFFER_SIZE_2D - PAYLOAD_SIZE) / 2.0);
        
                                            ANGLE_STEP_2D = (((double)BASE_ANGLE_2D / (double)(DATASET_SIZE_2D - 1)));
                                            ANGLE_POINT_2D = ((double)BASE_ANGLE_2D / 2) * RADIAN;

                                            //ROS_ERROR("\n2D BUFFER SET-UP COMPLETED >>\n\tBUFFER SIZE: %d\n\tSET SIZE: %d\n\tANGLE STEP: %.2f\n\tANGLE POINT: %.2f", \
                                            DATABUFFER_SIZE_2D, DATASET_SIZE_2D, \
                                            ANGLE_STEP_2D, ANGLE_POINT_2D);

                                            cloudBuffer_2D = new uint8_t[DATABUFFER_SIZE_2D - PAYLOAD_SIZE];
                                            cloudSetBuffer_2D = new float[DATASET_SIZE_2D];

                                            scan_laser->header.frame_id = frame_id;

                                            scan_laser->angle_min = -ANGLE_POINT_2D;
                                            scan_laser->angle_max = ANGLE_POINT_2D;
                                            scan_laser->angle_increment = ANGLE_STEP_2D * RADIAN;

                                            scan_laser->range_min = 0.1;
                                            scan_laser->range_max = ((double)BASE_DEPTH_2D * DIVISOR);

                                            scan_laser->ranges.resize(DATASET_SIZE_2D);
                                            scan_laser->intensities.resize(DATASET_SIZE_2D);
                                            
                                            scan_2D.get()->points.resize(DATASET_SIZE_2D);

                                            buffer_setup_2d = true;
                                        }
                                        
                                        last_time_laser = ros::Time::now();
                                        bufferPtr_2D = &bufferPtr[0];

                                        cloudScatter_2D(current_time_laser, last_time_laser, ANGLE_STEP_2D);
                                        //ROS_ERROR("2D ==> %d [%d, %d]",\
                                        (int)(bufferPtr[DATA_1] << 8 | bufferPtr[DATA_2]), DATABUFFER_SIZE_2D, DATASET_SIZE_2D);
                                        break;
                                    case 0x08: // 3D
                                        if (!buffer_setup_3d)
                                        {
                                            DATABUFFER_SIZE_3D = (int)(bufferPtr[DATA_1] << 8 | bufferPtr[DATA_2]) + PAYLOAD_SIZE;
                                            float BYTESET_RATIO_3D = (2.0 / 3.0);
                                            DATASET_SIZE_3D = (int)((float)(DATABUFFER_SIZE_3D - PAYLOAD_SIZE) * BYTESET_RATIO_3D);
                                            cloudBuffer = new uint8_t[DATABUFFER_SIZE_3D - PAYLOAD_SIZE];
                                            cloudSetBuffer = new float[DATASET_SIZE_3D];

                                            scan_3D.get()->points.resize(DATASET_SIZE_3D);

                                            buffer_setup_3d = true;
                                        }

                                        switch (currentBuffer)
                                        {
                                            case 0x00:
                                                bufferPtr01 = &bufferPtr[0];
                                                currentBuffer = 0x01;
                                                break;
                                            case 0x01:
                                                bufferPtr02 = &bufferPtr[0];
                                                currentBuffer = 0x00;
                                                break;
                                        }
                                        cloudScatter_3D();
                                        //ROS_ERROR("3D ==> %d [%d, %d]",\
                                        (int)(bufferPtr[DATA_1] << 8 | bufferPtr[DATA_2]), DATABUFFER_SIZE_3D, DATASET_SIZE_3D);
                                        break;
                                }
                                break;
                            case 0x00: // passed through header 1
                                current_time_laser = ros::Time::now(); 
                                break;
                            default:
                                break;
                        }
                    }
                }

				inProgress = 0x00;
            }
        }
        laser.close();
    }
    catch (boost::system::system_error ex)
    {
        ROS_ERROR("[Error] instantiating laser object. \
        Are you sure you have the correct port and baud rate? \
        Error was %s", ex.what());
    }
}

int main(int argc, char **argv)
{
    // Initialize ROS
    ros::init(argc, argv, "line_laser");

    std::thread first(running);
    first.join();

    return 0;
}
