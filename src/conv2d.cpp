#include <dlprim/operators.hpp>
#include <dlprim/cpu/cpu_ops.hpp>
#include <dlprim/gpu/program_cache.hpp>
#include <dlprim/utils/json_helpers.hpp>
#include <dlprim/json.hpp>
#include <cblas.h>
#include <boost/compute/event.hpp>

namespace dlprim {
   
    Convolution2DConfig Convolution2DConfig::from_json(json::value const &v)
    {
        Convolution2DConfig cfg;
        cfg.channels_in = v.get("channels_in",cfg.channels_in);
        cfg.channels_out = v.get<int>("channels_out");
        utils::get_1dNd_from_json(v,"kernel",cfg.kernel,true);
        utils::get_1dNd_from_json(v,"stride",cfg.stride);
        utils::get_1dNd_from_json(v,"dilate",cfg.dilate);
        utils::get_1dNd_from_json(v,"pad",cfg.pad);
        cfg.groups = v.get("groups",cfg.groups);
        cfg.bias = v.get("bias",cfg.bias);
        cfg.activation = utils::activation_from_json(v); 
        return cfg;
    }
    

    Shape Convolution2D::get_output_shape(Shape const &in)
    {
        DLPRIM_CHECK(in.size() == 4);
        int batch = in[0];
        DLPRIM_CHECK(in[1] == config_.channels_in);
        int ihw[2] = { in[2], in[3] };
        int ohw[2];
        for(int i=0;i<2;i++)        
            ohw[i] = (ihw[i] + 2 * config_.pad[i] - config_.dilate[i] * (config_.kernel[i] - 1) - 1) /  config_.stride[i] + 1;
        DLPRIM_CHECK(ohw[0] > 0);
        DLPRIM_CHECK(ohw[1] > 0);
        return Shape(batch,config_.channels_out,ohw[0],ohw[1]);
    }

    int Convolution2D::get_im2col_width()
    {
        return config_.channels_in / config_.groups * config_.kernel[0] * config_.kernel[1];
    }


    Convolution2D::Convolution2D(Context &ctx,Convolution2DConfig const &cfg,CalculationsMode mode) :
        OperatorWithParameters(ctx,mode),
        config_(cfg),
        dtype_(float_data)
    {
        DLPRIM_CHECK(config_.channels_out > 0);
        DLPRIM_CHECK(dtype_==float_data);
        DLPRIM_CHECK(config_.groups == 1);
    }
    void Convolution2D::setup(std::vector<TensorSpecs> const &in,
                              std::vector<TensorSpecs> &out,
                              size_t &workspace)
    {
        DLPRIM_CHECK(in.size() == 1);
        Shape in_shape = in[0].shape();
        DLPRIM_CHECK(in_shape.size() == 4);
        int chn   = in_shape[1];
        if(config_.channels_in == -1) {
            config_.channels_in = chn;
        }

        DLPRIM_CHECK(config_.channels_in  % config_.groups == 0);
        DLPRIM_CHECK(config_.channels_out % config_.groups == 0);

        Shape output_shape = get_output_shape(in_shape);
        out.assign({TensorSpecs(output_shape,dtype_)});

        std::vector<TensorSpecs> params;
        Shape params_shape(config_.channels_out,
                           config_.channels_in / config_.groups,
                           config_.kernel[0],
                           config_.kernel[1]);
        
        params.push_back(TensorSpecs(params_shape,dtype_));
        if(config_.bias) 
            params.push_back(TensorSpecs(Shape(config_.channels_out),dtype_));
        setup_parameters(std::move(params));

        ws_size_ = workspace = get_im2col_width() * output_shape[2] * output_shape[3] * size_of_data_type(dtype_);

        if(ctx_.is_cpu_context())
            return;

        cl::Program const &prog1 = gpu::Cache::instance().get_program(ctx_,"sgemm",
                                        "BIAS", (config_.bias ? 1 : 0),
                                        "BTRANS",1,
                                        "ACTIVATION",int(config_.activation));

        gemm_kernel_ = cl::Kernel(prog1,"sgemm");
        cl::Program const &prog2 = gpu::Cache::instance().get_program(ctx_,"im2col",
                                        "KERN_H",config_.kernel[0],
                                        "KERN_W",config_.kernel[1],
                                        "PAD_H",config_.pad[0],
                                        "PAD_W",config_.pad[1],
                                        "STRIDE_H",config_.stride[0],
                                        "STRIDE_W",config_.stride[1],
                                        "DILATE_H",config_.dilate[0],
                                        "DILATE_W",config_.dilate[1]);

        im2col_kernel_ = cl::Kernel(prog2,"im2col");

    }

    void Convolution2D::reshape(std::vector<Shape> const &in,
                               std::vector<Shape> &out)
    {
        DLPRIM_CHECK(in.size() == 1);
        out.assign({get_output_shape(in[0])});
    }

    void Convolution2D::forward(std::vector<Tensor> &in,std::vector<Tensor> &out,
            ExecutionContext const &ectx)
    {
        DLPRIM_CHECK(in.size() == 1);
        DLPRIM_CHECK(out.size() == 1);
        Shape in_shape = in[0].shape();
        Shape out_shape = out[0].shape();
        DLPRIM_CHECK(out_shape == get_output_shape(in_shape));
        DLPRIM_CHECK(parameters().size()==(1u+unsigned(config_.bias)));
        DLPRIM_CHECK(parameters()[0].shape() == Shape(config_.channels_out,config_.channels_in / config_.groups,config_.kernel[0],config_.kernel[1]));
        if(config_.bias)
            DLPRIM_CHECK(parameters()[1].shape() == Shape(config_.channels_out)); 

        if(ctx_.is_cpu_context())
            forward_cpu(in[0],out[0]);
        else
            forward_gpu(in[0],out[0],ectx);
    }

    void Convolution2D::forward_gpu(Tensor &in,Tensor &out,ExecutionContext const &ec)
    {
        int batch = in.shape()[0];

        int ind = 0;
        int img_offset_index;
        im2col_kernel_.setArg(ind++,config_.channels_in / config_.groups);
        im2col_kernel_.setArg(ind++,in.shape()[2]);
        im2col_kernel_.setArg(ind++,in.shape()[3]);

        im2col_kernel_.setArg(ind++,out.shape()[2]);
        im2col_kernel_.setArg(ind++,out.shape()[3]);
        im2col_kernel_.setArg(ind++,in.device_buffer());
        img_offset_index = ind++;
        im2col_kernel_.setArg(ind++,workspace_.device_buffer());
        im2col_kernel_.setArg(ind++,int(workspace_.device_offset()));

        ind = 0;
        int M = config_.channels_out;
        int N = out.shape()[2]*out.shape()[3];
        int K = get_im2col_width(); 
        gemm_kernel_.setArg(ind++,M);
        gemm_kernel_.setArg(ind++,N);
        gemm_kernel_.setArg(ind++,K);
        gemm_kernel_.setArg(ind++,parameters()[0].device_buffer());
        gemm_kernel_.setArg(ind++,int(parameters()[0].device_offset()));
        gemm_kernel_.setArg(ind++,K);
        gemm_kernel_.setArg(ind++,workspace_.device_buffer());
        gemm_kernel_.setArg(ind++,int(workspace_.device_offset()));
        gemm_kernel_.setArg(ind++,K);
        gemm_kernel_.setArg(ind++,out.device_buffer());
        int omg_offset_index = ind++;
        gemm_kernel_.setArg(ind++,N);
        if(config_.bias) {
            Tensor &bias = parameters()[1];
            gemm_kernel_.setArg(ind++,bias.device_buffer());
            gemm_kernel_.setArg(ind++,int(bias.device_offset()));
        }
        for(int i=0;i<batch;i++) {
            int inp_offset = i * in.shape().size_no_batch() + in.device_offset();
            int out_offset = i * out.shape().size_no_batch() + out.device_offset();

            im2col_kernel_.setArg(img_offset_index,inp_offset);
            gemm_kernel_.setArg(omg_offset_index,out_offset);

            ExecutionContext ec1 = ec.generate_series_context(i*2+0,batch*2);
            ExecutionContext ec2 = ec.generate_series_context(i*2+1,batch*2);
            
            ec.queue().enqueueNDRangeKernel(im2col_kernel_,cl::NullRange,
                                                           cl::NDRange(config_.channels_out,
                                                                       out.shape()[2],
                                                                       out.shape()[3]),
                                                           cl::NullRange,
                                                           ec1.events(),ec1.event("im2col",i));


            
            constexpr int tile_size = 128;
            constexpr int block_size = 8;
            int ls = tile_size / block_size; // blocksize/tile-size
            int gs0 = (M + tile_size - 1) / tile_size * tile_size / block_size;
            int gs1 = (N + tile_size - 1) / tile_size * tile_size / block_size;
            cl::NDRange global(gs0,gs1);
            cl::NDRange local(ls,ls);

            ec.queue().enqueueNDRangeKernel(gemm_kernel_,cl::NullRange,global,local,
                            ec2.events(),
                            ec2.event("gemm",i));
        }
    }

    void Convolution2D::im2col(Shape const &in,Shape const &outs,float *img_in,float *mat_in)
    {
        int kern_h   = config_.kernel[0];
        int kern_w   = config_.kernel[1];
        int pad_h    = config_.pad[0];
        int pad_w    = config_.pad[1];
        int dilate_h = config_.dilate[0];
        int dilate_w = config_.dilate[1];
        int stride_h = config_.stride[0];
        int stride_w = config_.stride[1];

        int rows = outs[2];
        int cols = outs[3];
        int src_rows = in[2];
        int src_cols = in[3];
        for(int chan = 0;chan < config_.channels_in;chan ++) {
            for(int r=0;r<rows;r++) {
                for(int c=0;c<cols;c++) {
                    int mat_row = r * cols + c;
                    int mat_col = chan * (kern_h * kern_w);
                    float *mat = mat_in + mat_row * config_.channels_in * (kern_h * kern_w) + mat_col;
                    int y_pos = -pad_h + r * stride_h;
                    int x_pos = -pad_w + c * stride_w;
                    float *img = img_in + src_cols * (chan * src_rows + y_pos) + x_pos;

                    for(int dy = 0;dy < kern_h * dilate_h ;dy+= dilate_h, img += src_cols * dilate_h) {
                        int y = y_pos + dy;
                        if(y >= 0 && y < src_rows) {
                            for(int dx=0;dx < kern_w * dilate_w ;dx+= dilate_w) {
                                int x = x_pos + dx;
                                *mat++ = (x >= 0 && x < src_cols) ? img[dx] : 0;
                            }
                        }
                        else {
                            for(int dx=0;dx < kern_w * dilate_w ;dx+= dilate_w) {
                                *mat++ = 0;
                            }
                        }
                    }
                }
            }
        }
    }

    void Convolution2D::forward_cpu(Tensor &in,Tensor &out)
    {
        int batch = in.shape()[0];
        float *imcols = static_cast<float *>(workspace_.host_data());
        float *kernel = parameters()[0].data<float>();
        int im2col_rows = out.shape()[2]*out.shape()[3];
        int kernel_cols = config_.channels_in * config_.kernel[0] * config_.kernel[1];
        int in_size_no_batch = in.shape().size_no_batch();
        int out_size_no_batch = out.shape().size_no_batch();
        for(int b=0;b<batch;b++) {
            float *img = in.data<float>() + in_size_no_batch*b;
            float *omg = out.data<float>() + out_size_no_batch*b;
            im2col(in.shape(),out.shape(),img,imcols);
			cblas_sgemm(CblasRowMajor,CblasNoTrans, CblasTrans,
					config_.channels_out,im2col_rows,kernel_cols,
					1.0f,
					kernel,kernel_cols,
					imcols,kernel_cols,
					0.0f,
					omg,
					im2col_rows);
            if(config_.bias) {
                float *bias = parameters()[1].data<float>();
                int plane_size = out.shape()[2]*out.shape()[3];
                for(int i=0;i<config_.channels_out;i++) {
                    cblas_saxpy(plane_size,1.0f,bias + i,0,omg + plane_size*i,1);
                }
            }
        }
        cpu::apply_activation(out.data<float>(),out.shape().total_size(),config_.activation);
    }
    void Convolution2D::backward_data(std::vector<Tensor> &,
                                   std::vector<Tensor> &,
                                   std::vector<Tensor> &,
                                   std::vector<Tensor> &,
                                   ExecutionContext const &)
    {
        throw NotImplementedError("Convolition2D::backward_data");
    }
        
    void Convolution2D::backward_param(std::vector<Tensor> &,
                                std::vector<Tensor> &,
                                std::vector<Tensor> &,
                                std::vector<Tensor> &,
                                ExecutionContext const &)
    {
        throw NotImplementedError("Convolition2D::backward_param");
    }
} // dlprim
