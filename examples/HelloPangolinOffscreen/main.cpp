#include <pangolin/pangolin.h>
#include <opencv/highgui.h>
#include <opencv2/opencv.hpp>

int main( int /*argc*/, char** /*argv*/ )
{
    pangolin::CreateWindowAndBind("Main",640,480,pangolin::Params({{"scheme", "headless"}}));
    glEnable(GL_DEPTH_TEST);

    // Define Projection and initial ModelView matrix
    pangolin::OpenGlRenderState s_cam(
        pangolin::ProjectionMatrix(640,480,420,420,320,240,0.2,100),
        pangolin::ModelViewLookAt(-2,2,-2, 0,0,0, pangolin::AxisY)
    );

    // Create Interactive View in window
    pangolin::Handler3D handler(s_cam);
    pangolin::View& d_cam = pangolin::CreateDisplay()
            .SetBounds(0.0, 1.0, 0.0, 1.0, -640.0f/480.0f)
            .SetHandler(&handler);

    pangolin::SaveWindowOnRender("window");

    glClearColor(0.25f, 0.25f, 0.5f, 1.0f);
    // Clear screen and activate view to render into
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    d_cam.Activate(s_cam);

    // Render OpenGL Cube
    pangolin::glDrawColouredCube();

    // Swap frames and Process Events
    pangolin::FinishFrame();


    unsigned char* image = (unsigned char*)malloc(sizeof(unsigned char) * 3 * 640 * 480);
    glReadPixels(0, 0, 640, 480, GL_RGB, GL_UNSIGNED_BYTE, image);

    cv::Mat cv_image(480,640,CV_8UC3,image);
    cv::imwrite("cv_output.png", cv_image);

    pangolin::QuitAll();

    return 0;
}
