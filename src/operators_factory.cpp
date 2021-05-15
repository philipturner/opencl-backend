#include <dlprim/operator.hpp>
#include <dlprim/functions.hpp>
#include <dlprim/json.hpp>
#include <functional>

namespace dlprim {
    
static std::map<std::string,std::function<Operator *(Context &,json::value const &,DataType )> > generators = {
    { 
        "SoftMax", 
        [](Context &ctx,json::value const &p,DataType dt) {
            return new SoftMax(ctx,SoftMaxConfig::from_json(p),dt);
        }
    },
    {
        "Elementwise", 
        [](Context &ctx,json::value const &p,DataType dt) {
            return new Elementwise(ctx,ElementwiseConfig::from_json(p),dt);
        }
    },
    {
        "Pooling2D", 
        [](Context &ctx,json::value const &p,DataType dt) {
            return new Pooling2D(ctx,Pooling2DConfig::from_json(p),dt);
        }
    }
};
    
std::unique_ptr<Operator> create_by_name(Context &ctx,
                                        std::string const &name,
                                        json::value const &parameters,DataType dtype)
{
    auto p=generators.find(name);
    if(p == generators.end()) {
        throw ValidatioError("Unknown operator " + name);
    }
    std::unique_ptr<Operator> r(p->second(ctx,parameters,dtype));
    return r;

}

} /// namespace
