#include <dlprim/ops/bwd_bias.hpp>
#include <dlprim/gpu/program_cache.hpp>
#include <my_cblas.hpp>
#include <iostream>

namespace dlprim {
    BWBias::~BWBias() {}

    BWBias::BWBias(Context &ctx,int batch,int rows_columns,DataType dt) :
        ctx_(ctx),
        batch_(batch),
        rows_columns_(rows_columns)
    {
        DLPRIM_CHECK(dt == float_data);
        int total_size = rows_columns_;
        if(ctx_.is_cpu_context())
            return;

        if(total_size <= 64)
            wg_ = 64;
        else if(total_size <= 128)
            wg_ = 128;
        else
            wg_ = 256;
        items_per_wi_ = (total_size + wg_ - 1) / wg_;

        cl::Program const &prog1 = gpu::Cache::instance().get_program(ctx_,"bwd_bias",
                                        "WG_SIZE",wg_,
                                        "ITEMS_PER_WI",items_per_wi_,
                                        "SIZE_2D",rows_columns_
                                        );
        kernel_ = cl::Kernel(prog1,"bwd_bias");
        cl::Program const &prog2 = gpu::Cache::instance().get_program(ctx_,"scal");
        scal_ = cl::Kernel(prog2,"sscal");
    }

    void BWBias::backward_gpu(Tensor &dy,Tensor &dw,float beta,ExecutionContext const &e)
    {
        int b = dy.shape()[0];
        int features = dw.shape()[0];
        cl::NDRange l(wg_,1,1);
        cl::NDRange g(wg_,b,features);

        auto ec1 = e.generate_series_context(0,2);
        auto ec2 = e.generate_series_context(1,2);
        int p=0;
        scal_.setArg(p++,features);
        scal_.setArg(p++,beta);
        scal_.setArg(p++,dw.device_buffer());
        scal_.setArg(p++,int(dw.device_offset()));

        p=0;
        kernel_.setArg(p++,b);
        kernel_.setArg(p++,features);
        kernel_.setArg(p++,dy.device_buffer());
        kernel_.setArg(p++,int(dy.device_offset()));
        kernel_.setArg(p++,dw.device_buffer());
        kernel_.setArg(p++,int(dw.device_offset()));

        e.queue().enqueueNDRangeKernel(scal_,cl::NullRange,cl::NDRange(features),cl::NullRange,ec1.events(),ec1.event("bwd_bias_scal")); 
        e.queue().enqueueNDRangeKernel(kernel_,cl::NullRange,g,l,ec2.events(),ec2.event("bwd_bias"));
    }
    void BWBias::backward_cpu(Tensor &dy,Tensor &dw,float beta)
    {
        int batches = dy.shape()[0];
        int features = dw.shape()[0];
        float *dyp = dy.data<float>();
        float *dwp = dw.data<float>();
        if(beta == 0)
            memset(dwp,0,features*sizeof(float));
        else
            cblas_sscal(features,beta,dwp,1);
        if(rows_columns_ == 1) {
            for(int b=0;b<batches;b++) {
                cblas_saxpy(features,1.0,dyp,1,dwp,1);
                dyp+=features;
            }
        }
        else {
            for(int b=0;b<batches;b++) {
                for(int f=0;f<features;f++) {
                    for(int pix=0;pix<rows_columns_;pix++) {
                        dwp[f] += *dyp++;
                    }
                }
            }
        }
    }

}