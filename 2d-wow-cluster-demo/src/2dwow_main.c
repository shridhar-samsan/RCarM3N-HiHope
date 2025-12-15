/*
****************************************************************************
  Copyright (c) [2021-2022] Renesas Electronics Corporation
  This software is released under the MIT License.
  http://opensource.org/licenses/mit-license.php
****************************************************************************
*/
/*******************************************************************************
   Title: Main Module

   This module contains the main and the main loop functions.
 */

#include "rcar-xos/osal/r_osal.h"
#include "r_wm_config.h"
#include "r_wm_api.h"

#include "r_print_api.h"
#include "r_prr_api.h"

#include "r_config_drw2d.h"
#include "r_drw2d_types.h"
#include "r_drw2d_os.h"
#include "r_drw2d_api.h"
#include "r_drw2d_ctx_dhd.h"

#include "2dwow.h"
#include "2dwow_main.h"
#include "demo.h"
#include "app_cluster.h"

extern osal_memory_manager_handle_t    g_hndl_mmngr;

#define ALLOC_SIZE  (1024u * 1024u * 32u)
#define MUTEX_TIMEOUT_MS   (1000u) /* 1000 milisecond */

static davehd_dev_t dhd_dev_obj;
davehd_dev_t *dhd_dev = &dhd_dev_obj;

static r_drw2d_Framebuffer_t loc_fb_obj;
r_drw2d_Framebuffer_t *fb = &loc_fb_obj;

#define MAX_NR_FBUF    2
#define ALLOC_ALIGN    128
static osal_memory_buffer_handle_t     VidMemHandle[MAX_NR_FBUF] = { NULL, };

static void *loc_VidFramebuffer[MAX_NR_FBUF] = { NULL, };

static int32_t prim_dsp;

/* Surface information of Surface 1 & 2 */
static r_wm_Surface_t loc_Surf = {
    .PosX = 0,
    .PosY = 0,
    .PosZ = 0,
    .Width = 0,              /* will be filled by the API */
    .Height = 0,             /* will be filled by the API */
    .StrideY = 0,
    .StrideC = 0,
    .Fmt = R_WM_FMT_ARGB8888,
    .Handle = 0,             /* will be filled by the API */
    .FBuf = 0
};

extern int  demo_init(davehd_dev_t *Dev);
extern int  demo_render(davehd_dev_t *Dev, int first, int state);
extern void demo_shutdown(davehd_dev_t *Dev);


static int32_t loc_WmInit(int32_t Disp, r_wm_Surface_t *Surf)
{
    r_wm_Error_t    wmErr;
    int32_t         ret = 0;
    static uint32_t bckg_color = 0x00000000u; /* Black */
    uint32_t        max_nr_layers;

    /* Initialise the WM Device */
    wmErr = R_WM_DevInit(Disp);
    if (R_WM_ERR_SUCCESS == wmErr) {

        /* Same colour format as the surface */
        wmErr = R_WM_ScreenPropertySet(Disp, R_WM_SCREEN_PROP_FMT, (void *)&(Surf->Fmt));
        if (R_WM_ERR_SUCCESS != wmErr) {
            ret = -1;
            R_PRINT_Log("R_WM_ScreenPropertySet, FMT Set failed Err= %d\n", wmErr);
            goto deinit;
        }
        wmErr = R_WM_ScreenPropertySet(Disp, R_WM_SCREEN_PROP_BKGCOLOR, (void *)&bckg_color);
        if (R_WM_ERR_SUCCESS != wmErr) {
            ret = -1;
            R_PRINT_Log("R_WM_ScreenPropertySet, BKGCOLOR Set failed Err= %d\n", wmErr);
            goto deinit;
        }

        /* Get the resolution */
        wmErr = R_WM_DevInfoGet(Disp, &max_nr_layers, &(Surf->Width), &(Surf->Height));
        if (R_WM_ERR_SUCCESS != wmErr) {
            ret = -1;
            R_PRINT_Log("R_WM_DevInfoGet FAILED= %d\n", wmErr);
            goto deinit;
        }
        if (0 == Surf->StrideY) {
            Surf->StrideY = Surf->Width * 4;
            Surf->StrideC = Surf->StrideY;
        }

        /* enable Screen */
        wmErr = R_WM_ScreenEnable(Disp, 1);
        if (R_WM_ERR_SUCCESS != wmErr) {
            ret = -1;
            R_PRINT_Log("R_WM_ScreenEnable failed Err= %d\n", wmErr);
            goto deinit;
        }
    } else {
        ret = -1;
        R_PRINT_Log("R_WM_DevInit failed Err= %d\n", wmErr);
        goto end;
    }

    /* The display may be larger than the surface that we actually use */
    if (Surf->Height > LOC_MAX_DISP_HEIGHT ) {
        Surf->Height = LOC_MAX_DISP_HEIGHT;
    }

    /* Initialise the surfaces */
    wmErr = R_WM_SurfaceGet(Disp, Surf);
    if (R_WM_ERR_SUCCESS != wmErr) {
        ret = -1;
        R_PRINT_Log("R_WM_SurfaceGet failed Err= %d\n", wmErr);
        goto deinit;
    } else {
        R_PRINT_Log("R_WM_SurfaceGet. Handle = %p\n", Surf->Handle);
    }

    wmErr = R_WM_SurfacePropertySet(Disp, R_WM_SURF_PROP_POS, Surf);
    if (R_WM_ERR_SUCCESS != wmErr) {
        ret = -1;
        wmErr = R_WM_SurfaceRelease(Disp, Surf);
        R_PRINT_Log("R_WM_SurfacePropertySet: set position failed Err= %d\n", wmErr);
        goto deinit;
    }

    wmErr = R_WM_SurfacePropertySet(Disp, R_WM_SURF_PROP_SIZE, Surf);
    if (R_WM_ERR_SUCCESS != wmErr) {
        ret = -1;
        wmErr = R_WM_SurfaceRelease(Disp, Surf);
        R_PRINT_Log("R_WM_SurfacePropertySet: set Size failed Err= %d\n", wmErr);
        goto deinit;
    } else {
        goto end;
    }

deinit:
    wmErr = R_WM_DevDeinit(Disp);
    if (R_WM_ERR_SUCCESS != wmErr) {
        R_PRINT_Log("R_WM_DevDeinit failed Err= %d\n", wmErr);
    }
end:
    return ret;
}

int32_t Drw2DInit(void)
{
    int32_t ret = 0;

    ret |= R_DRW2D_Init();
    ret |= R_DRW2D_Open(0, 0, &dhd_dev->sys, &dhd_dev->drw2d);
    ret |= R_DRW2D_NativeDriverBegin(dhd_dev->drw2d, R_DRW2D_NATIVEDRV_KEEPSTATE);
    ret |= R_DRW2D_NativeDriverHandleGet(dhd_dev->drw2d, &dhd_dev->dhd);
    if (ret) {
        R_PRINT_Log("error init drw2d \r\n");
        return 1;
    }
    return 0;
}

int32_t Drw2DDeinit(void)
{
    R_DRW2D_NativeDriverEnd(dhd_dev->drw2d, R_DRW2D_NATIVEDRV_APPCONTEXT);
    return 0;
}


/*******************************************************************************
*/
int32_t AppInit(void)
{
    int32_t ret = 0;
    e_osal_return_t osal_ret;
    size_t i;
    size_t size;
    r_prr_DevName_t soc;

    R_PRINT_Log("CR7 START\r\n");

    soc = R_PRR_GetDevice();
    switch (soc) {
        case R_PRR_RCARE3:
            /* LVDS0 */
            prim_dsp = R_WM_DEV_VOUT0;
            break;
        default:
            /* HDMI0 */
            prim_dsp = R_WM_DEV_VOUT1;
            break;
    }

    R_PRINT_Log("about WM init\r\n");
    ret = loc_WmInit(prim_dsp, &loc_Surf);
    if (-1 == ret) {
        R_PRINT_Log("WM init err\r\n");
    }
    else {
        if ( 0 != g_hndl_mmngr ) {
            size = loc_Surf.Width * loc_Surf.Height * 4;
            for ( i = 0 ; i < MAX_NR_FBUF ; i++ ) {
                osal_ret = R_OSAL_MmngrAlloc( g_hndl_mmngr, size, ALLOC_ALIGN, &VidMemHandle[i] );
                if(OSAL_RETURN_OK != osal_ret) {
                    R_PRINT_Log("Error R_OSAL_MmngrAlloc[%d] failer, ret = %d \n", i, osal_ret);
                    ret = -1;
                }
                else {
                    osal_ret = R_OSAL_MmngrGetHwAddr( VidMemHandle[i], OSAL_AXI_BUS_ID_IPA_MAIN_MEMORY, 
                                   (uintptr_t*)&loc_VidFramebuffer[i] );
                    if(OSAL_RETURN_OK != osal_ret) {
                        R_PRINT_Log("Error R_OSAL_MmngrGetHwAddr failer, ret = %d \n", osal_ret);
                        ret = -1;
                    }
                }
            }
        }
        else {
            R_PRINT_Log("\n\nError/MMNGR!!\n\n");
            ret = -1;
        }
    }
    R_PRINT_Log("WM init done\r\n");

    if (-1 != ret) {
        Drw2DInit();

        demo_init(dhd_dev);

        /* Init framebuffer object */
        fb->Flags = 0;
        fb->Handle = 0;
        fb->Buffer.Pitch = loc_Surf.Width;
        fb->Buffer.Size.Width = loc_Surf.Width;
        fb->Buffer.Size.Height = loc_Surf.Height;
        fb->Buffer.PixelFormat = R_DRW2D_PIXELFORMAT_ARGB8888;
        fb->Buffer.Data = 0;

        R_PRINT_Log("App Init done\r\n");
    }
    return ret;
}

void AppDeInit(unsigned int ret)
{
    r_wm_Error_t wmErr;

    /* Start exiting the the application */
    /* release the surfaces */
    wmErr = R_WM_SurfaceRelease(prim_dsp, &loc_Surf);
    if (R_WM_ERR_SUCCESS != wmErr) {
        R_PRINT_Log("R_WM_SurfaceRelease failed Err= %d\n", wmErr);
    }

    wmErr = R_WM_DevDeinit(prim_dsp);
    if (R_WM_ERR_SUCCESS != wmErr) {
        R_PRINT_Log("R_WM_DevDeinit failed Err= %d\n", wmErr);
    }
    R_PRINT_Log("loc_MainLoop END, ret = %d \r\n", ret);
    demo_shutdown(dhd_dev);

    Drw2DDeinit();
    return;
}

static int64_t AppLoop(void *Arg)
{
    uint32_t cnt;
    int32_t ret = 0;
    r_wm_Error_t wmErr = 0;

    ret = AppInit();

    for (cnt = 0; (ret == 0 && wmErr == 0); cnt++) {
        int idx = (cnt % 2 == 1);

        fb->Buffer.Data =    loc_VidFramebuffer[idx];
        ret |= R_DRW2D_FramebufferSet(dhd_dev->drw2d, fb);
        demo_set_fb(fb);
        demo_render(dhd_dev, (cnt == 0), 0);
        ret |= R_DRW2D_GpuFinish(dhd_dev->drw2d, R_DRW2D_FINISH_WAIT); /* why do the finish ?? */

        loc_Surf.FBuf = (uint32_t)loc_VidFramebuffer[idx];
        wmErr |= R_WM_SurfacePropertySet(prim_dsp, R_WM_SURF_PROP_BUFFER, &loc_Surf);
        if (wmErr != 0)
        {
            R_PRINT_Log("R_WM_SurfacePropertySet(), set buffer Err= %d\n", wmErr);
        }

        wmErr |= R_WM_ScreenSurfacesFlush(prim_dsp, 1);
    }

    AppDeInit(ret);

    return 0;
}

static int64_t main_thread(void *Arg)
{
    e_osal_return_t osal_ret;
    int64_t return_value;
    st_osal_thread_config_t thread_config;
    osal_thread_handle_t thread;

    r_dhd_ServerInfo_t            dhdInitInfo;

    R_PRINT_Log("===START 2d cluster wow on DHD===\n");

    /* Start DHD Server. */
    dhdInitInfo.VideoMemoryHandle = (dhd_handle_t)g_hndl_mmngr;
    dhdInitInfo.VideoMemorySize   = ALLOC_SIZE;
    dhdInitInfo.TimeOut           = MUTEX_TIMEOUT_MS;

    dhd_server_main(&dhdInitInfo);

    /* Threads init */
    osal_ret = R_OSAL_ThreadInitializeThreadConfigSt( &thread_config );
    if( OSAL_RETURN_OK != osal_ret )
    {
        ClusterError(-1);
    }

    thread = OSAL_THREAD_HANDLE_INVALID;
    
    thread_config.priority   = LOC_MAINLOOP_PRIO;
    thread_config.task_name  = "MainLoop";
    thread_config.func       = (p_osal_thread_func_t)AppLoop;
    thread_config.userarg    = (void*)0;
    thread_config.stack_size = LOC_COM_THREAD_STACK_SIZE;
    
    osal_ret = R_OSAL_ThreadCreate( &thread_config,
                                    (osal_thread_id_t)LOC_OSAL_THREAD_DRW2D_MAINLOOP,
                                    &thread );
    if( OSAL_RETURN_OK != osal_ret )
    {
        ClusterError(-1);
    }

    /* The code below will wait for DHD and application threads to exit */
    R_OSAL_ThreadJoin( thread, &return_value );

    R_PRINT_Log("end all threads\n\r");
    return 0;
}

int x2dwow_main(void)
{
    st_osal_thread_config_t thread_config;
    osal_thread_handle_t thread;

    thread_config.priority   = LOC_XMAINLOOP_PRIO;
    thread_config.task_name  = "main_thread";
    thread_config.func       = (p_osal_thread_func_t)main_thread;
    thread_config.userarg    = (void*)0;
    thread_config.stack_size = LOC_XMAINLOOP_STACKSIZE;

    R_OSAL_ThreadCreate(&thread_config,
                        (osal_thread_id_t)LOC_OSAL_THREAD_DRW2D_MAINTHREAD,
                        &thread);
    return 0;
}

// DEBUG Code
#if 0

/***********************************************************
  Function: DHD_SYS_SQRTF
  Returns a single-precision floating point number of the square-root result
*/

float64_t DHD_SYS_SQRTF(float64_t val)
{
    float64_t result;
    result = (float64_t) sqrt(val);
    return result;
}

#endif


