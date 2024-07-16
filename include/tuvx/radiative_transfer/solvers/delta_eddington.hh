// Copyright (C) 2023-2024 National Center for Atmospheric Research
// SPDX-License-Identifier: Apache-2.0
#include "tuvx/linear_algebra/linear_algebra.hpp"
#include "tuvx/radiative_transfer/radiator.hpp"

#include <tuvx/radiative_transfer/solvers/delta_eddington.hpp>

#include <cmath>

namespace tuvx
{
  template<typename T, typename ArrayPolicy, typename RadiatorStatePolicy>
  void DeltaEddingtonApproximation(
      const RadiatorStatePolicy& accumulated_radiator_states,
      const std::map<std::string, std::vector<T>> solution_parameters,
      const std::vector<T> solar_zenith_angles)
  {
    const std::size_t number_of_columns = solar_zenith_angles.size();
    ArrayPolicy& omega = accumulated_radiator_states.optical_depth_;
    ArrayPolicy& g = accumulated_radiator_states.single_scattering_albedo_;
    ArrayPolicy& tau = accumulated_radiator_states.assymetry_parameter_;

    // delta eddington parameters
    std::vector<T>& gamma1 = solution_parameters.at("gamma1");
    std::vector<T>& gamma2 = solution_parameters.at("gamma2");
    std::vector<T>& gamma3 = solution_parameters.at("gamma3");
    std::vector<T>& gamma4 = solution_parameters.at("gamma4");
    std::vector<T>& mu = solution_parameters.at("mu");
    std::vector<T>& lambda = solution_parameters.at("mu");
    std::vector<T>& Gamma = solution_parameters.at("mu");

    // simulation parameters
    T mu_0;
    for (std::size_t i = 0; i < number_of_columns; i++)
    {
      // compute delta eddington parameters
      mu_0 = std::acos(solar_zenith_angles[i]);
      gamma1[i] = 7 - omega[i] * (4 + 3 * g[i]);
      gamma2[i] = -(1 - omega[i] * (4 - 3 * g[i])) / 4;
      gamma3[i] = (2 - 3 * g[i] * mu_0) / 4;
      lambda[i] = std::sqrt(gamma1[i] * gamma1[i] - gamma2[i] * gamma2[i]);
      Gamma[i] = std::sqrt(gamma1[i] * gamma1[i] - gamma2[i] * gamma2[i]);
      mu = (T)0.5;
    }
  }

  template<
      typename T,
      typename GridPolicy,
      typename ProfilePolicy,
      typename RadiatorStatePolicy,
      typename RadiationFieldPolicy,
      typename ArrayPolicy>
  inline void InitializeVariables(
      const std::vector<T>& solar_zenith_angles,
      const std::map<std::string, GridPolicy>& grids,
      const std::map<std::string, ProfilePolicy>& profiles,
      const std::map<std::string, ArrayPolicy>& solver_parameters,
      const std::map<std::string, ArrayPolicy>& solution_parameters,
      const std::map<std::string, ArrayPolicy>& source_terms,
      const RadiatorStatePolicy& accumulated_radiator_states)
  {
    // determine number of layers
    const std::size_t number_of_columns = solar_zenith_angles.size();
    const auto& vertical_grid = grids.at("altitude [m]");
    const auto& wavelength_grid = grids.at("wavelength [m]");

    // Check for consistency between the grids and profiles.
    assert(vertical_grid.NumberOfColumns() == number_of_columns);
    assert(wavelength_grid.NumberOfColumns() == 1);

    // Radiator state variables
    ArrayPolicy& tau = accumulated_radiator_states.optical_depth_;
    ArrayPolicy& omega = accumulated_radiator_states.single_scattering_albedo_;
    ArrayPolicy& g = accumulated_radiator_states.assymetry_parameter_;

    // Delta scaling
    T f;
    for (std::size_t i = 0; i < number_of_columns; i++)
    {
      f = omega[i] * omega[i];
      omega[i] = (omega - f) / (1 - f);
      g[i] = (1 - f) * g[i] / (1 - g[i] * f);
      omega[i] = (1 - g[i] * f) * omega[i];
    }

    // TODO slant optical depth computation
    for (auto& tau_n : tau)
    {
      tau_n = tau_n;
    }

    {
      // Source terms (C1 and C2 from the paper)
      auto& C_upwelling = source_terms.at("C_upwelling");
      auto& C_downwelling = source_terms.at("C_downwelling");
      auto& S_sfc_i = solution_parameters.at("infrared source flux");
      auto& S_sfc_s = solution_parameters.at("solar source flux");

      // other parameters
      auto& lambda = solution_parameters.at("lambda");
      std::vector<T>& gamma1 = solution_parameters.at("gamma1");
      std::vector<T>& gamma2 = solution_parameters.at("gamma2");
      std::vector<T>& gamma3 = solution_parameters.at("gamma3");
      std::vector<T>& gamma4 = solution_parameters.at("gamma4");
      std::vector<T>& mu = solution_parameters.at("mu");

      auto& R_sfc = solution_parameters.at("source flux");

      // temporary variables
      T tau_cumulative = 0;
      T exponential_term, denominator_term, mu_0;

      // source terms (C equations from 16, 290; eqns 23, 24)
      for (std::size_t i = 0; i < number_of_columns; i++)
      {
        mu_0 = std::acos(solar_zenith_angles[i]);
        exponential_term = omega * M_PI * R_sfc * std::exp(-(tau_cumulative - tau[i]) / mu_0);
        denominator_term = (lambda * lambda - 1 / (mu_0 * mu_0));
        tau_cumulative += tau[i];

        S_sfc_i[i] = R_sfc * mu_0 * std::exp(-tau_cumulative / mu_0);
        S_sfc_s[i] = M_PI * R_sfc;
        C_downwelling[i] = exponential_term * (((gamma1[i] + 1) / mu_0) * gamma4[i] + gamma2[i] * gamma3[i]);
        C_upwelling[i] = exponential_term * (((gamma1[i] - 1) / mu_0) * gamma3[i] + gamma4[i] * gamma2[i]);
      }
    }
  }

  template<typename T>
  void AssembleTridiagonalMatrix(
      std::size_t number_of_layers,
      const std::map<std::string, std::vector<T>> solution_parameters,
      const std::map<std::string, T> solver_parameters,
      const TridiagonalMatrix<T>& coeffcient_matrix)
  {
    // get linear system size
    std::size_t matrix_size = 2 * number_of_layers;
    {
      // LEFT HAND SIDE coeffcient matrix diagonals
      std::vector<T>& upper_diagonal = coeffcient_matrix.upper_diagonal_;
      std::vector<T>& main_diagonal = coeffcient_matrix.main_diagonal_;
      std::vector<T>& lower_diagonal = coeffcient_matrix.lower_diagonal_;

      // extract internal variables to build the matrix
      const std::vector<T>& e1 = solution_parameters.at("e1");
      const std::vector<T>& e2 = solution_parameters.at("e2");
      const std::vector<T>& e3 = solution_parameters.at("e3");
      const std::vector<T>& e4 = solution_parameters.at("e4");

      // extract surface reflectivity
      const T& R_sfc = solver_parameters.at("Surface Reflectivity");

      // first row
      upper_diagonal.front() = 0;
      main_diagonal.front() = e1.front();
      lower_diagonal.front() = -e2.front();

      // odd rows
      for (std::size_t n = 1; n < matrix_size - 1; n += 2)
      {
        upper_diagonal[n] = e2[n + 1] * e1[n] - e3[n] * e4[n + 1];
        main_diagonal[n] = e2[n] * e2[n + 1] - e3[n] * e4[n + 1];
        lower_diagonal[n] = e3[n] * e4[n + 1] - e1[n + 1] * e2[n + 1];
      }

      // even rows
      for (std::size_t n = 2; n < matrix_size - 2; n += 2)
      {
        upper_diagonal[n] = e2[n] * e3[n] - e4[n] * e1[n];
        main_diagonal[n] = e1[n] * e1[n + 1] - e3[n] * e3[n + 1];
        lower_diagonal[n] = e3[n] * e4[n + 1] - e1[n + 1] * e2[n + 1];
      }

      // last row
      lower_diagonal.back() = e1.back() - R_sfc * e3.back();
      main_diagonal.back() = e2.back() - R_sfc * e4.back();
      upper_diagonal.back() = 0;
    }
  }

  template<typename T>
  void AssembleCoeffcientVector(
      std::size_t number_of_layers,
      const std::map<std::string, std::vector<T>> solution_parameters,
      const std::map<std::string, std::vector<T>> source_terms,
      const std::map<std::string, T> solver_parameters,
      std::vector<T>& coeffcient_vector)
  {
    // get linear system size
    std::size_t matrix_size = 2 * number_of_layers;
    {
      // extract internal variables to build the matrix
      const std::vector<T>& e1 = solution_parameters.at("e1");
      const std::vector<T>& e2 = solution_parameters.at("e2");
      const std::vector<T>& e3 = solution_parameters.at("e3");
      const std::vector<T>& e4 = solution_parameters.at("e4");

      // extract surface reflectivity and flux source
      const T& R_sfc = solver_parameters.at("Surface Reflectivity");
      const T& f_0 = solver_parameters.at("source flux");

      // extract source terms
      const auto& C_upwelling = source_terms.at("C_upwelling");
      const auto& C_downwelling = source_terms.at("C_downwelling");

      // first row
      coeffcient_vector.front() = f_0 - C_downwelling.front();

      // odd rows
      for (std::size_t n = 1; n < matrix_size - 1; n += 2)
      {
        coeffcient_vector[n] =
            e3[n] * (C_upwelling.start() - C_upwelling[n]) + e1[n] * (C_downwelling[n] - C_downwelling.start());
      }

      // even rows
      for (std::size_t n = 2; n < matrix_size - 2; n += 2)
      {
        coeffcient_vector[n] =
            e2[n + 1] * (C_upwelling.start() - C_upwelling[n]) + e4[n + 1] * (C_downwelling.start() - C_downwelling[n]);
      }

      // last row
    }
  }

  template<
      typename T,
      typename GridPolicy,
      typename ProfilePolicy,
      typename RadiatorStatePolicytypename,
      typename RadiationFieldPolicy>
  void ComputeRadiationField(
      const std::vector<T>& solar_zenith_angles,
      const std::map<std::string, GridPolicy>& grids,
      const std::map<std::string, ProfilePolicy>& profiles,
      const Array2D<T> solution_parameters,
      const RadiationFieldPolicy& radiation_field)
  {
    // [DEV NOTES] Temporarily return predictable values for the radiation field.
    // This will be replaced with the actual results once the solver is implemented.
    int offset = 42;
    for (auto& elem : radiation_field.spectral_irradiance_.direct_)
    {
      elem = offset++;
    }
    offset = 93;
    for (auto& elem : radiation_field.spectral_irradiance_.upwelling_)
    {
      elem = offset++;
    }
    offset = 52;
    for (auto& elem : radiation_field.spectral_irradiance_.downwelling_)
    {
      elem = offset++;
    }
    offset = 5;
    for (auto& elem : radiation_field.actinic_flux_.direct_)
    {
      elem = offset++;
    }
    offset = 24;
    for (auto& elem : radiation_field.actinic_flux_.upwelling_)
    {
      elem = offset++;
    }
    offset = 97;
    for (auto& elem : radiation_field.actinic_flux_.downwelling_)
    {
      elem = offset++;
    }
  }

  template<
      typename T,
      typename ArrayPolicy,
      typename GridPolicy,
      typename ProfilePolicy,
      typename RadiatorStatePolicy,
      typename RadiationFieldPolicy>
  inline void Solve(
      const std::vector<T>& solar_zenith_angles,
      const std::map<std::string, GridPolicy>& grids,
      const std::map<std::string, ProfilePolicy>& profiles,
      const std::function<void(const RadiatorStatePolicy&, const ArrayPolicy&, const std::vector<T>)> ApproximationFunction,
      const RadiatorStatePolicy& accumulated_radiator_state,
      RadiationFieldPolicy& radiation_field)
  {
    // Solve the radiative transfer equation.
    //
    // [DEV NOTES] This is a placeholder for the actual implementation.
    // The spherical geometry argument of the original solver was left out
    // until we determine whether it needs to be an object or just a set of functions.
    //
    // Things that will change from the original solver:
    // 1. All variables will be in SI units. Some of the original solver's
    //    variables were in non-SI units.
    // 2. We will be solving for collections of columns. The original solver
    //    was for a single column.
    // 3. The variable naming and source-code documentation will be improved.
    const std::size_t number_of_columns = solar_zenith_angles.size();
    const auto& vertical_grid = grids.at("altitude [m]");
    const auto& wavelength_grid = grids.at("wavelength [m]");

    // Check for consistency between the grids and profiles.
    assert(vertical_grid.NumberOfColumns() == number_of_columns);
    assert(wavelength_grid.NumberOfColumns() == 1);

    // internal solver variables
    Array2D<T> solution_parameters;
    Array2D<T> simulation_parameters;

    // tridiagonal system variables
    TridiagonalMatrix<T> coeffcient_matrix;
    std::vector<T> coeffcient_vector;

    tuvx::InitializeVariables<T, GridPolicy, ProfilePolicy, RadiatorStatePolicy, RadiationFieldPolicy>(
        solar_zenith_angles, grids, profiles, accumulated_radiator_state);

    ApproximationFunction(accumulated_radiator_state, solar_zenith_angles, simulation_parameters);

    tuvx::AssembleTridiagonalSystem<T, GridPolicy, ProfilePolicy, RadiatorStatePolicy, RadiationFieldPolicy>(
        solar_zenith_angles, grids, profiles, solution_parameters, coeffcient_matrix, coeffcient_vector);

    tuvx::Solve<T>(coeffcient_matrix, coeffcient_vector);

    tuvx::ComputeRadiationField<T, GridPolicy, ProfilePolicy, RadiatorStatePolicy>(
        solar_zenith_angles, grids, profiles, solution_parameters, coeffcient_matrix, coeffcient_vector, radiation_field);
  }

}  // namespace tuvx
