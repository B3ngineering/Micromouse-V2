#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/laser_scan.hpp>
#include <geometry_msgs/msg/twist.hpp>
#include <nav_msgs/msg/odometry.hpp>
#include <tf2/LinearMath/Quaternion.h>
#include <tf2/LinearMath/Matrix3x3.h>
#include <chrono>
#include <thread>
#include <vector>
#include <queue>
#include <climits>

using namespace std;

class FloodFill : public rclcpp::Node {
public:
    FloodFill() : Node("Flood_Fill"), state_(State::MOVING_FORWARD), initial_yaw_(0.0), target_yaw_(0.0), distance_traveled_(0.0) {
        // Subscribe to Lidar data
        laser_subscriber_ = this->create_subscription<sensor_msgs::msg::LaserScan>(
            "/laser_controller/out", 5, 
            bind(&FloodFill::laser_callback, this, placeholders::_1));

        // Subscribe to Odometry data
        odom_subscriber_ = this->create_subscription<nav_msgs::msg::Odometry>(
            "/odom", 5, 
            bind(&FloodFill::odom_callback, this, placeholders::_1));
        
        // Create a publisher for the robot's velocity
        publisher_ = this->create_publisher<geometry_msgs::msg::Twist>("/cmd_vel", 10);

        // Set a timer to periodically call the move_robot function
        timer_ = this->create_wall_timer(
            chrono::milliseconds(10), bind(&FloodFill::move_mouse, this));

        RCLCPP_INFO(this->get_logger(), "Flood fill node initialized.");

        initialize_maze();
    }

private:

    // Maze information
    const int MAZE_SIZE_ = 20;
    vector<vector<int>> maze_;
    int goal_x_ = 19;
    int goal_y_ = 19;

    void initialize_maze () {
        // Resize the maze grid to MAZE_SIZE x MAZE_SIZE and fill with Manhattan distances
        maze_.resize(MAZE_SIZE_, vector<int>(MAZE_SIZE_, 0));

        for (int x = 0; x < MAZE_SIZE_; ++x) {
            for (int y = 0; y < MAZE_SIZE_; ++y) {
                // Set each cell to its Manhattan distance from the goal cell (19, 19)
                maze_[x][y] = abs(goal_x_ - x) + abs(goal_y_ - y);
            }
        }
        // Print the maze with its values
        for (int x = 0; x < MAZE_SIZE_; ++x) {
            for (int y = 0; y < MAZE_SIZE_; ++y) {
            cout << maze_[x][y] << " ";
            }
            cout << endl;
        }
        
    }


    enum class State {
        MOVING_FORWARD,
        DETECTING_WALL,
        TURNING,
        REFINING_TURN
    };

    void laser_callback(const sensor_msgs::msg::LaserScan::SharedPtr msg) {
        if (state_ == State::DETECTING_WALL) {
            // Update the maze based on laser scan data
            const float safe_distance = 0.75;
            int x = static_cast<int>(current_x_);
            int y = static_cast<int>(current_y_);

            if (msg->ranges[0] < safe_distance) {
                maze_[x][y + 1] = INT_MAX; // Front wall
            }
            if (msg->ranges[1] < safe_distance) {
                maze_[x - 1][y] = INT_MAX; // Left wall
            }
            if (msg->ranges[3] < safe_distance) {
                maze_[x + 1][y] = INT_MAX; // Right wall
            }

            state_ = State::MOVING_FORWARD;
        }
    }

    void odom_callback(const nav_msgs::msg::Odometry::SharedPtr msg) {
        // Update current position and yaw
        RCLCPP_INFO(this->get_logger(), "Current position: (%f, %f)", current_x_, current_y_);
        RCLCPP_INFO(this->get_logger(), "New position: (%f, %f)", msg->pose.pose.position.x, msg->pose.pose.position.y);
        tf2::Quaternion q(
            msg->pose.pose.orientation.x,
            msg->pose.pose.orientation.y,
            msg->pose.pose.orientation.z,
            msg->pose.pose.orientation.w);
        
        tf2::Matrix3x3 m(q);
        double roll, pitch, yaw;
        m.getRPY(roll, pitch, yaw);

        current_yaw_ = yaw;

        // Update odometry with distance from last step
        double new_x = msg->pose.pose.position.x;
        double new_y = msg->pose.pose.position.y;

        distance_traveled_ += sqrt(pow(new_x - current_x_, 2) + pow(new_y - current_y_, 2));

        current_x_ = new_x;
        current_y_ = new_y;
    }

    void move_mouse() {
        auto twist = geometry_msgs::msg::Twist();

        // Check if the robot has reached the goal
        if ((goal_x_ - 0.5 <= current_x_ && current_x_ <= goal_x_ + 0.5) && (goal_y_ - 0.5 <= current_y_ && current_y_ <= goal_y_ + 0.5)) {
            twist.linear.x = 0.0;
            twist.angular.z = 0.0;
            publisher_->publish(twist);
            RCLCPP_INFO(this->get_logger(), "Goal reached!");
            rclcpp::shutdown();
        }

        switch (state_) {
            case State::MOVING_FORWARD: {
                RCLCPP_INFO(this->get_logger(), "Moving forward. Distance traveled: %f", distance_traveled_);

                // Take a step without taking any action
                if (distance_traveled_ < 0.99) {
                    twist.linear.x = 0.5;
                    twist.angular.z = 0.0;
                } else {
                    twist.linear.x = 0.0;
                    RCLCPP_INFO(this->get_logger(), "0.5 meter traveled. Checking for walls.");
                    
                    // Pause and switch state
                    state_ = State::DETECTING_WALL;
                    auto stop_twist = geometry_msgs::msg::Twist();
                    stop_twist.linear.x = 0.0;
                    stop_twist.angular.z = 0.0;
                    publisher_->publish(stop_twist);
                    rclcpp::sleep_for(chrono::seconds(1));
                }
                break;
            }

            case State::TURNING: {
                double angle_difference = normalize_angle(target_yaw_ - current_yaw_);
                const double tolerance = 0.01;

                // Calculate angle to turn the robot
                if (fabs(angle_difference) > tolerance) {
                    twist.angular.z = 1.0 * (angle_difference > 0 ? 1.0 : -1.0);
                    twist.linear.x = 0.0;
                } else {
                    twist.angular.z = 0.0;
                    state_ = State::REFINING_TURN;
                    RCLCPP_INFO(this->get_logger(), "Turn complete. Refining turn.");
                }
                break;
            }

            // Refine the turn to be more precise and prevent drift
            case State::REFINING_TURN: {
                double angle_difference = normalize_angle(target_yaw_ - current_yaw_);
                const double fine_tolerance = 0.001;

                if (fabs(angle_difference) > fine_tolerance) {
                    twist.angular.z = 0.2 * (angle_difference > 0 ? 1.0 : -1.0);
                    twist.linear.x = 0.0;
                } else {
                    twist.angular.z = 0.0;
                    state_ = State::MOVING_FORWARD;
                    distance_traveled_ = 0.0;
                    RCLCPP_INFO(this->get_logger(), "Refinement complete. Moving forward.");
                }
                break;
            }

            case State::DETECTING_WALL:
                flood_fill();
                break;
        }

        publisher_->publish(twist);
    }

    void flood_fill() {
        state_ = State::TURNING;
    }

    double normalize_angle(double angle) {
        while (angle > M_PI) angle -= 2.0 * M_PI;
        while (angle < -M_PI) angle += 2.0 * M_PI;
        return angle;
    }

    rclcpp::Publisher<geometry_msgs::msg::Twist>::SharedPtr publisher_;
    rclcpp::Subscription<sensor_msgs::msg::LaserScan>::SharedPtr laser_subscriber_;
    rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr odom_subscriber_;
    rclcpp::TimerBase::SharedPtr timer_;

    State state_;
    bool wall_detected_;
    double initial_yaw_;
    double current_yaw_;
    double target_yaw_;
    double current_x_;
    double current_y_;
    double distance_traveled_;

};

int main(int argc, char *argv[]) {
    rclcpp::init(argc, argv);
    rclcpp::spin(std::make_shared<FloodFill>());
    rclcpp::shutdown();
    return 0;
}