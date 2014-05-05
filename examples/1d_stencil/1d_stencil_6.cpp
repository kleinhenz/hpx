//  Copyright (c) 2014 Hartmut Kaiser
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

// This is the sixth in a series of examples demonstrating the development of a
// fully distributed solver for a simple 1D heat distribution problem.
//
// This example builds on example five.

#include <hpx/hpx_init.hpp>
#include <hpx/hpx.hpp>

#include <boost/shared_array.hpp>

///////////////////////////////////////////////////////////////////////////////
double k = 0.5;     // heat transfer coefficient
double dt = 1.;     // time step
double dx = 1.;     // grid spacing

inline std::size_t idx(std::size_t i, std::size_t size)
{
    return (boost::int64_t(i) < 0) ? (i + size) % size : i % size;
}

inline std::size_t locidx(std::size_t i, std::size_t np, std::size_t nl)
{
    return i / (np/nl);
}

///////////////////////////////////////////////////////////////////////////////
struct partition_data
{
private:
    typedef hpx::util::serialize_buffer<double> buffer_type;

public:
    partition_data()
      : size_(0)
    {}

    // Create a new (uninitialized) partition of the given size.
    partition_data(std::size_t size)
      : data_(new double [size], size, buffer_type::take),
        size_(size),
        min_index_(0)
    {}

    // Create a new (initialized) partition of the given size.
    partition_data(std::size_t size, double initial_value)
      : data_(new double [size], size, buffer_type::take),
        size_(size),
        min_index_(0)
    {
        double base_value = double(initial_value * size);
        for (std::size_t i = 0; i != size; ++i)
            data_[i] = base_value + double(i);
    }

    // Create a partition which acts as a proxy to a part of the embedded array.
    // The proxy is assumed to refer to either the left or the right boundary
    // element.
    partition_data(partition_data const& base, std::size_t min_index)
      : data_(base.data_.data()+min_index, 1, buffer_type::reference),
        size_(base.size()),
        min_index_(min_index)
    {
        HPX_ASSERT(min_index < base.size());
    }

    double& operator[](std::size_t idx) { return data_[index(idx)]; }
    double operator[](std::size_t idx) const { return data_[index(idx)]; }

    std::size_t size() const { return size_; }

private:
    std::size_t index(std::size_t idx) const
    {
        HPX_ASSERT(idx >= min_index_ && idx < size_);
        return idx - min_index_;
    }

private:
    // Serialization support: even if all of the code below runs on one
    // locality only, we need to provide an (empty) implementation for the
    // serialization as all arguments passed to actions have to support this.
    friend class boost::serialization::access;

    template <typename Archive>
    void serialize(Archive& ar, const unsigned int version)
    {
        ar & data_ & size_ & min_index_;
    }

private:
    buffer_type data_;
    std::size_t size_;
    std::size_t min_index_;
};

std::ostream& operator<<(std::ostream& os, partition_data const& c)
{
    os << "{";
    for (std::size_t i = 0; i != c.size(); ++i)
    {
        if (i != 0)
            os << ", ";
        os << c[i];
    }
    os << "}";
    return os;
}

///////////////////////////////////////////////////////////////////////////////
// This is the server side representation of the data. We expose this as a HPX
// component which allows for it to be created and accessed remotely through
// a global address (hpx::id_type).
struct partition_server
  : hpx::components::simple_component_base<partition_server>
{
    enum partition_type
    {
        left_partition, middle_partition, right_partition
    };

    // construct new instances
    partition_server() {}

    partition_server(partition_data const& data)
      : data_(data)
    {}

    partition_server(std::size_t size, double initial_value)
      : data_(size, initial_value)
    {}

    // Access data. The parameter specifies what part of the data should be
    // accessed. As long as the result is used locally, no data is copied,
    // however as soon as the result is requested from another locality only
    // the minimally required amount of data will go over the wire.
    partition_data get_data(partition_type t) const
    {
        switch (t)
        {
        case left_partition:
            return partition_data(data_, data_.size()-1);

        case middle_partition:
            break;

        case right_partition:
            return partition_data(data_, 0);

        default:
            HPX_ASSERT(false);
            break;
        }
        return data_;
    }

    // Every member function which has to be invoked remotely needs to be
    // wrapped into a component action. The macro below defines a new type
    // 'get_data_action' which represents the (possibly remote) member function
    // partition::get_data().
    HPX_DEFINE_COMPONENT_CONST_DIRECT_ACTION(partition_server, get_data, get_data_action);

private:
    partition_data data_;
};

// The macros below are necessary to generate the code required for exposing
// our partition type remotely.
//
// HPX_REGISTER_MINIMAL_COMPONENT_FACTORY() exposes the component creation
// through hpx::new_<>().
typedef hpx::components::simple_component<partition_server> partition_server_type;
HPX_REGISTER_MINIMAL_COMPONENT_FACTORY(partition_server_type, partition_server);

// HPX_REGISTER_ACTION() exposes the component member function for remote
// invocation.
typedef partition_server::get_data_action get_data_action;
HPX_REGISTER_ACTION(get_data_action);

///////////////////////////////////////////////////////////////////////////////
// This is a client side helper class allowing to hide some of the tedious
// boilerplate.
struct partition : hpx::components::client_base<partition, partition_server>
{
    typedef hpx::components::client_base<partition, partition_server> base_type;

    partition() {}

    // Create new component on locality 'where' and initialize the held data
    partition(hpx::id_type where, std::size_t size, double initial_value)
      : base_type(hpx::new_<partition_server>(where, size, initial_value))
    {}

    // Create a new component on the locality co-located to the id 'where'. The
    // new instance will be initialized from the given partition_data.
    partition(hpx::id_type where, partition_data const& data)
      : base_type(hpx::new_colocated<partition_server>(where, data))
    {}

    // Attach a future representing a (possibly remote) partition.
    partition(hpx::future<hpx::id_type> && id)
      : base_type(std::move(id))
    {}

    // Unwrap a future<partition> (a partition already holds a future to the
    // id of the referenced object, thus unwrapping accesses this inner future).
    partition(hpx::future<partition> && c)
      : base_type(std::move(c))
    {}

    ///////////////////////////////////////////////////////////////////////////
    // Invoke the (remote) member function which gives us access to the data.
    // This is a pure helper function hiding the async.
    hpx::future<partition_data> get_data(partition_server::partition_type t) const
    {
        partition_server::get_data_action act;
        return hpx::async(act, get_gid(), t);
    }
};

///////////////////////////////////////////////////////////////////////////////
struct stepper
{
    // Our data for one time step
    typedef std::vector<partition> space;

    // Our operator
    static double heat(double left, double middle, double right)
    {
        return middle + (k*dt/dx*dx) * (left - 2*middle + right);
    }

    // The partitioned operator, it invokes the heat operator above on all elements
    // of a partition.
    static partition_data heat_part_data(partition_data const& left,
        partition_data const& middle, partition_data const& right)
    {
        std::size_t size = middle.size();
        partition_data next(size);

        next[0] = heat(left[size-1], middle[0], middle[1]);

        for (std::size_t i = 1; i != size-1; ++i)
            next[i] = heat(middle[i-1], middle[i], middle[i+1]);

        next[size-1] = heat(middle[size-2], middle[size-1], right[0]);

        return next;
    }

    static partition heat_part(partition const& left, partition const& middle,
        partition const& right)
    {
        using hpx::lcos::local::dataflow;
        using hpx::util::unwrapped;

        return dataflow(
            unwrapped(
                [left, middle, right](partition_data const& l, partition_data const& m,
                    partition_data const& r)
                {
                    // The new partition_data will be allocated on the same locality
                    // as 'middle'.
                    return partition(middle.get_gid(), heat_part_data(l, m, r));
                }
            ),
            left.get_data(partition_server::left_partition),
            middle.get_data(partition_server::middle_partition),
            right.get_data(partition_server::right_partition)
        );
    }

    // do all the work on 'np' partitions, 'nx' data points each, for 'nt'
    // time steps
    space do_work(std::size_t np, std::size_t nx, std::size_t nt);
};

// Global functions can be exposed as actions as well. That allows to invoke
// those remotely. The macro HPX_PLAIN_ACTION() defines a new action type
// 'heat_part_action' which wraps the global function heat_part(). It can be
// used to call that function on a given locality.
HPX_PLAIN_ACTION(stepper::heat_part, heat_part_action);

///////////////////////////////////////////////////////////////////////////////
// do all the work on 'np' partitions, 'nx' data points each, for 'nt'
// time steps
stepper::space stepper::do_work(std::size_t np, std::size_t nx, std::size_t nt)
{
    using hpx::lcos::local::dataflow;
    using hpx::util::unwrapped;
    using hpx::util::placeholders::_1;
    using hpx::util::placeholders::_2;
    using hpx::util::placeholders::_3;

    std::vector<hpx::id_type> localities = hpx::find_all_localities();
    std::size_t nl = localities.size();                    // Number of localities

    // U[t][i] is the state of position i at time t.
    std::vector<space> U(2);
    for (space& s: U)
        s.resize(np);

    // Initial conditions: f(0, i) = i
    for (std::size_t i = 0; i != np; ++i)
        U[0][i] = partition(localities[locidx(i, np, nl)], nx, double(i));

    heat_part_action act;
    for (std::size_t t = 0; t != nt; ++t)
    {
        space const& current = U[t % 2];
        space& next = U[(t + 1) % 2];

        for (std::size_t i = 0; i != np; ++i)
        {
            // we execute the action on the locality of the middle partition
            auto Op = hpx::util::bind(act, localities[locidx(i, np, nl)], _1, _2, _3);
            next[i] = dataflow(Op, current[idx(i-1, np)], current[i], current[idx(i+1, np)]);
        }
    }

    // Return the solution at time-step 'nt'.
    return U[nt % 2];
}

///////////////////////////////////////////////////////////////////////////////
int hpx_main(boost::program_options::variables_map& vm)
{
    boost::uint64_t np = vm["np"].as<boost::uint64_t>();   // Number of partitions.
    boost::uint64_t nx = vm["nx"].as<boost::uint64_t>();   // Number of grid points.
    boost::uint64_t nt = vm["nt"].as<boost::uint64_t>();   // Number of steps.

    std::vector<hpx::id_type> localities = hpx::find_all_localities();
    std::size_t nl = localities.size();                    // Number of localities

    if (np < nl)
    {
        std::cout << "The number of partitions should not be smaller than "
                     "the number of localities" << std::endl;
        return hpx::finalize();
    }

    // Create the stepper object
    stepper step;

    // Execute nt time steps on nx grid points and print the final solution.
    stepper::space solution = step.do_work(np, nx, nt);

    // Print the solution at time-step 'nt'.
    for (std::size_t i = 0; i != np; ++i)
    {
        std::cout << "U[" << i << "] = "
                  << solution[i].get_data(partition_server::middle_partition).get()
                  << std::endl;
    }

    return hpx::finalize();
}

int main(int argc, char* argv[])
{
    using namespace boost::program_options;

    options_description desc_commandline;
    desc_commandline.add_options()
        ("nx", value<boost::uint64_t>()->default_value(10),
         "Local x dimension (of each partition)")
        ("nt", value<boost::uint64_t>()->default_value(45),
         "Number of time steps")
        ("np", value<boost::uint64_t>()->default_value(10),
         "Number of partitions")
        ("k", value<double>(&k)->default_value(0.5),
         "Heat transfer coefficient (default: 0.5)")
        ("dt", value<double>(&dt)->default_value(1.0),
         "Timestep unit (default: 1.0[s])")
        ("dx", value<double>(&dx)->default_value(1.0),
         "Local x dimension")
    ;

    // Initialize and run HPX
    return hpx::init(desc_commandline, argc, argv);
}
