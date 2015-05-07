/**
 * @file /src/qnode.cpp
 *
 * @brief Ros communication central!
 **/

/*****************************************************************************
** Includes
*****************************************************************************/

#include <sstream>
#include "../include/ranger_librarian_gui/qnode.hpp"
#include <ctime>
#include <iomanip>

/*****************************************************************************
** Namespaces
*****************************************************************************/

namespace ranger_librarian_gui {

/*****************************************************************************
** Implementation
*****************************************************************************/

QNode::QNode(int argc, char** argv ) :
	init_argc(argc),
    init_argv(argv),
    read_label_(false), read_label_success_(false), weight_max_reached_(false),
    lr_(OCR_FRAME_SKIP, QUEUE_MAX_LENGTH, QUEUE_ACCEPT_RATE),
    q_user_image_(QImage()),
    action_last_(NAVIGATOR_STOP),
    action_current_(NAVIGATOR_MOVE)
    {}

QNode::~QNode() {
    if(ros::isStarted()) {
      ros::shutdown(); // explicitly needed since we use ros::start();
      ros::waitForShutdown();
    }
	wait();
    // delete allocated objects
    delete  nh_;
    delete it_;
}

bool QNode::init() {
	ros::init(init_argc,init_argv,"ranger_librarian_gui");
	if ( ! ros::master::check() ) {
		return false;
	}
	ros::start(); // explicitly needed since our nodehandle is going out of scope.
    nh_ = new ros::NodeHandle("");
    it_ = new image_transport::ImageTransport( *nh_);

    string rgb_image_topic;
    string depth_low_duration;
    string depth_low_action;
    string scale_topic;
    string scale_filtered_topic;

    // get parameters for subscribers, if not use defaults
    nh_->param<string>("rgb_image", rgb_image_topic, RGB_IMAGE_TOPIC);
    nh_->param<string>("depth_low_duration", depth_low_duration, DEPTH_LOW_DURATION_TOPIC);
    nh_->param<string>("depth_low_action", depth_low_action,     DEPTH_BELOW_TIMER_TOPIC);

    nh_->param<string>("scale", scale_topic, SCALE_TOPIC);
    nh_->param<string>("scale_filtered", scale_filtered_topic, SCALE_FILTERED_TOPIC);




	// Add your ros communications here.
    chatter_publisher = nh_->advertise<std_msgs::String>("chatter", 1000);
    // Subscribers
    sub_rgb_    = it_->subscribe("/usb_cam/image_raw", 1, &QNode::rgb_callback, this);

    sub_depth_low_action_  =   nh_->subscribe<const std_msgs::String&>(depth_low_action, 1, &QNode::depth_low_action_callback, this);

    sub_scale_  =           nh_->subscribe<const std_msgs::Float64&>(scale_topic, 1, &QNode::scale_callback, this);
    sub_scale_filtered_  =  nh_->subscribe<const ranger_librarian::WeightFiltered&>(scale_filtered_topic, 1, &QNode::scale_filtered_callback, this);

    // get the rest of paramters
    nh_->param("weight_max_allowed", weight_max_allowed_,        double(5));
    nh_->param("time_depth_low_read", time_depth_low_read_,      double(1.5));
    nh_->param("time_wait_read_label", time_wait_read_label_,    double(6));
    nh_->param("time_wait_add_book", time_wait_add_book_,        double(8));

    ROS_INFO("qnode init finished");

    start();
	return true;
}

void QNode::run() {

    log("Run started move.");

    //the same as ros::spin();
    while (ros::ok()) {
        ros::getGlobalCallbackQueue()->callAvailable(ros::WallDuration(0.1));
    }

	std::cout << "Ros shutdown, proceeding to close the gui." << std::endl;
	Q_EMIT rosShutdown(); // used to signal the gui for a shutdown (useful to roslaunch)
}

void QNode::log(const std::string &msg) {

    logging_model.insertRows(0,1);
    std::stringstream msg_timestamped;

    std::time_t rawtime = std::time(nullptr);
    char buffer [9];
    std::strftime (buffer,9,"%T",localtime (&rawtime));

    msg_timestamped <<  "[" << string(buffer)  << "]: " << msg;
    QVariant new_row(QString(msg_timestamped.str().c_str()));

    logging_model.setData(logging_model.index(0),new_row);

    Q_EMIT navigatorActionStringUpdated(); // used to signal the gui for a string change
}


/// CALLBACKS
void QNode::rgb_callback(const sensor_msgs::ImageConstPtr &msg)
{
    if (DEBUG) {
        printf("rgb_callback msg received.\n");
    }

    try
    {
        // cv_bridge magic
        cv_ptr_ = cv_bridge::toCvShare(msg, sensor_msgs::image_encodings::RGB8);

        // process frame with orc if needed
        read_label_success_ = lr_.processFrame(cv_ptr_->image);

        // image must be copied since it uses the cv_ptr->image for storage which is asynchronously overwritten in the next callback invocation
        user_image_ = cv_ptr_->image.clone();

        // prepare label reader user output image view
        lr_.prepareUserImage(user_image_);

        q_user_image_ = QImage( user_image_.data, user_image_.cols, user_image_.rows, user_image_.step[0], QImage::Format_RGB888);

        Q_EMIT userImageUpdated();
    }
    catch (cv_bridge::Exception& e)
    {
        printf("cv_bridge exception: %s", e.what());
        return;
    }
}


void QNode::depth_low_action_callback(const std_msgs::String& msg) {

    string depth_low_action = msg.data;

    if (DEBUG) {
        printf("depth_low_action msg received:  %s\n", depth_low_action.c_str());
    }

    if (!read_label_ && depth_low_action.compare("stop")==0 ) {

        read_label_ = true;

        action_current_ = NAVIGATOR_STOP;
        log("Trying to read label...");
        //wait for book label
        if (book_read_label()) {

            log("Read label success! Waiting for book...");

            if (book_read_weight()) {
                log("Book added successfully!");
            } else {
                log("Book not added! Timeout...");
            }

        } else {
            log("Read label failed! Timeout...");
        }
        read_label_ = false;
        action_current_ = NAVIGATOR_MOVE;

    }

}

void QNode::scale_callback(const std_msgs::Float64& msg) {

    double weight_current = msg.data;

    if (DEBUG) {
        printf("Scale msg received:  %0.2f\n", weight_current);
    }

    if (! weight_max_reached_ && weight_current > weight_max_allowed_) {
        weight_max_reached_ = true;
        log("MAX WEIGHT reached! ");
        action_current_ = NAVIGATOR_FINISH;
    } else {
        weight_max_reached_ = false;
    }
    // implement control of max weight reached
}

void QNode::scale_filtered_callback(const ranger_librarian::WeightFiltered& msg) {

    double weight_stable = msg.weight_stable;
    double weight_change = msg.weight_change;
    bool weight_changed =  msg.weight_changed;

    if (DEBUG) {
        printf("scale_filtered_callback msg received:  %0.3f\n", weight_stable);
    }

    if (weight_changed) {
        if (weight_change > 0) {
            last_book_add_time_ = msg.change_time;
            string logstring = "Book added: \n" + last_book_add_.author + ", " + last_book_add_.callNumber;
            log(logstring);
        } else {
            log("Book removed");
        }
    } else {
        // no change
    }

}
/////////////////////////////////////////////////////////////////////
// OTHER methods
bool QNode::book_read_label() {

    read_label_success_ = false;
    bool read_success = false;

    ros::Time start_time = ros::Time::now();
    ros::Time run_time = ros::Time::now();

    lr_.reset();
    lr_.readLabel(true);

    while (!read_success && (run_time-start_time < ros::Duration(time_wait_read_label_)) && ros::ok() ) {

        read_success = read_label_success_;

        ros::getGlobalCallbackQueue()->callAvailable(ros::WallDuration(0.1));
        run_time = ros::Time::now();
    }

    if (read_success){
        //update last book values
        last_book_add_time_ = ros::Time::now();
        last_book_add_.author = lr_.getAuthor();
        last_book_add_.callNumber = lr_.getCallNumber();
        last_book_add_.weight = 0;
    } else {
        last_book_add_time_ = ros::Time(0);
        last_book_add_.weight = 0;
        last_book_add_.author = "";
        last_book_add_.callNumber = "";
    }

    return read_success;
}

bool QNode::book_read_weight() {

    bool book_added = false;

    ros::Time start_time = ros::Time::now();
    ros::Time run_time = ros::Time::now();

    while (!book_added && (run_time-start_time < ros::Duration(time_wait_read_label_)) && ros::ok() ) {

        if (last_book_add_time_ > start_time ) {
            book_added = true;
        }

        ros::getGlobalCallbackQueue()->callAvailable(ros::WallDuration(0.1));
        run_time = ros::Time::now();
    }

    return book_added;
}



}  // namespace ranger_librarian_gui
