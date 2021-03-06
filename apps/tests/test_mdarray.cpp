#include <sirius.h>

using namespace sirius;

mdarray<int, 1> f1()
{
    mdarray<int, 1> aa;
    aa = mdarray<int, 1>(4);
    for (int i = 0; i < 4; i++) aa(i) = 200 + i;
    return aa;
}

void f2()
{
    mdarray<int, 1> a1(4);
    for (int i = 0; i < 4; i++) a1(i) = 100 + i;

    mdarray<int, 1> a2 = f1();
    for (int i = 0; i < 4; i++)
    {
        std::cout << "a1(" << i << ")=" << a1(i) << std::endl;
        std::cout << "a2(" << i << ")=" << a2(i) << std::endl;
    }
    mdarray<int, 1> a3(std::move(a2));
//== 
//== //    a1.deallocate();
//== //
//== //    std::cout << "Deallocate a1" << std::endl;
//== //
//== //    for (int i = 0; i < 4; i++)
//== //    {
//== //        std::cout << "a2(" << i << ")=" << a2(i) << std::endl;
//== //    }
//== //
//== //
//== //    mdarray<int, 1> a3 = a2;
//== //    
    for (int i = 0; i < 4; i++)
    {
        std::cout << "a3(" << i << ")=" << a3(i) << std::endl;
    }
 
    mdarray<int, 1> a4;
    a4 = std::move(a3);

    a4 = mdarray<int, 1>(20);
    
    #ifndef NDEBUG
    std::cout << "Allocated memory : " << mdarray_mem_count::allocated().load() << std::endl;
    #endif
}

void f3()
{   
    for (int i = 0; i < 100; i++) {
        #pragma omp parallel
        {
            int tid = omp_get_thread_num();
            mdarray<double_complex, 2> a(100, 100);
            a(0, 0) = double_complex(tid, tid);
        }
        if (mdarray_mem_count::allocated().load() != 0) {
            printf("oops! mdarray_mem_count class is not thread safe\n");
        }
    }
}

void f4()
{
    mdarray<int, 1> buf;

    buf = mdarray<int, 1>(100, memory_t::host | memory_t::device, "buf");

    buf = mdarray<int, 1>(200, memory_t::host | memory_t::device, "buf");
    
    //buf = mdarray<int, 1>(300, memory_t::host | memory_t::device, "buf");


}

void f5()
{
    mdarray<double, 3> a;

    if (a.size(0) != 0 || a.size(1) != 0 || a.size(2) != 0) {
        printf("wrong sizes\n");
    }
}

template <typename T, int N>
void f6(mdarray<T, N>& a)
{
    std::array<mdarray_index_descriptor, N> dims;
    for (int i = 0; i < N; i++) {
        dims[i] = mdarray_index_descriptor(0, 10);
    }
    a = mdarray<T, N>(dims);
    a[0] = 100;
    a[a.size() - 1] = 200;
}

int main(int argn, char **argv)
{
    sirius::initialize(1);

    f2();

    f3();

    f4();

    f5();
    
    mdarray<double, 2> a;
    f6(a);

    #ifndef NDEBUG
    std::cout << "Allocated memory : " << mdarray_mem_count::allocated().load() << std::endl;
    #endif

    sirius::finalize();
}
