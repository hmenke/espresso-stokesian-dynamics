#include <iostream>
#include <vector>

#include "sd.hpp"
#include "sd_gpu.hpp"

std::vector<double> sd_gpu(std::vector<double> const &x_host,
                           std::vector<double> const &f_host,
                           std::size_t n_part) {
  sd::solver<policy::device, double> viscous_force{n_part};
  return viscous_force.calc_vel(x_host, f_host);
}
