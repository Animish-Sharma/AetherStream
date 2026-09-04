mod ffi;

use std::error::Error;
use std::ffi::CStr;
use std::fmt;

#[derive(Clone, Debug)]
pub struct Config {
    pub target_rate: f32,
    pub absolute_error_bound: f32,
    pub deadzone_factor: f32,
    pub enable_crc: bool,
    pub enable_index: bool,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            target_rate: 4.0,
            absolute_error_bound: 0.0,
            deadzone_factor: 0.3,
            enable_crc: true,
            enable_index: false,
        }
    }
}

impl Config {
    pub fn builder() -> ConfigBuilder {
        ConfigBuilder {
            config: Self::default(),
        }
    }
}

#[derive(Clone, Debug)]
pub struct ConfigBuilder {
    config: Config,
}

impl ConfigBuilder {
    pub fn target_rate(mut self, value: f32) -> Self {
        self.config.target_rate = value;
        self
    }

    pub fn absolute_error_bound(mut self, value: f32) -> Self {
        self.config.absolute_error_bound = value;
        self
    }

    pub fn deadzone_factor(mut self, value: f32) -> Self {
        self.config.deadzone_factor = value;
        self
    }

    pub fn enable_crc(mut self, value: bool) -> Self {
        self.config.enable_crc = value;
        self
    }

    pub fn enable_index(mut self, value: bool) -> Self {
        self.config.enable_index = value;
        self
    }

    pub fn build(self) -> Result<Config, AetherError> {
        validate_config(&self.config)?;
        Ok(self.config)
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum AetherError {
    InvalidArgument,
    BufferTooSmall { required: usize },
    CorruptedStream,
    RateBudgetExceeded,
    DeprecatedFormat,
    Internal,
    UnknownStatus(i32),
    ExpectedSampleCount { expected: usize, actual: usize },
}

impl fmt::Display for AetherError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidArgument => formatter.write_str("invalid AetherStream argument"),
            Self::BufferTooSmall { required } => {
                write!(
                    formatter,
                    "AetherStream output buffer requires {required} elements"
                )
            }
            Self::CorruptedStream => formatter.write_str("corrupted or unsupported AetherStream"),
            Self::RateBudgetExceeded => formatter.write_str("AetherStream rate budget exceeded"),
            Self::DeprecatedFormat => formatter.write_str("deprecated AetherStream wire format"),
            Self::Internal => formatter.write_str("internal AetherStream error"),
            Self::UnknownStatus(status) => {
                write!(formatter, "unknown AetherStream status {status}")
            }
            Self::ExpectedSampleCount { expected, actual } => write!(
                formatter,
                "expected {expected} AetherStream samples but frame contains {actual}"
            ),
        }
    }
}

impl Error for AetherError {}

fn status_error(status: ffi::AetherStatus, required: usize) -> AetherError {
    match status {
        ffi::AETHER_ERR_INVALID_ARG => AetherError::InvalidArgument,
        ffi::AETHER_ERR_BUFFER_TOO_SMALL => AetherError::BufferTooSmall { required },
        ffi::AETHER_ERR_CORRUPTED_STREAM => AetherError::CorruptedStream,
        ffi::AETHER_ERR_RATE_BUDGET_EXCEEDED => AetherError::RateBudgetExceeded,
        ffi::AETHER_ERR_DEPRECATED_FORMAT => AetherError::DeprecatedFormat,
        ffi::AETHER_ERR_INTERNAL => AetherError::Internal,
        value => AetherError::UnknownStatus(value),
    }
}

fn validate_config(config: &Config) -> Result<(), AetherError> {
    if !config.target_rate.is_finite()
        || !(2.0..=8.0).contains(&config.target_rate)
        || !config.absolute_error_bound.is_finite()
        || config.absolute_error_bound < 0.0
        || !config.deadzone_factor.is_finite()
        || config.deadzone_factor < 0.0
    {
        return Err(AetherError::InvalidArgument);
    }
    Ok(())
}

pub fn compress(input: &[f32], config: &Config) -> Result<Vec<u8>, AetherError> {
    validate_config(config)?;
    if input.is_empty() {
        return Err(AetherError::InvalidArgument);
    }
    let native = ffi::AetherConfig {
        target_rate: config.target_rate,
        absolute_error_bound: config.absolute_error_bound,
        deadzone_factor: config.deadzone_factor,
        enable_crc: u8::from(config.enable_crc),
        enable_index: u8::from(config.enable_index),
    };
    let capacity = if config.absolute_error_bound == 0.0 {
        let bound = (input.len() as f64 * f64::from(config.target_rate) / 8.0).ceil();
        if !bound.is_finite() || bound > usize::MAX as f64 {
            return Err(AetherError::InvalidArgument);
        }
        bound as usize
    } else {
        input
            .len()
            .checked_mul(16)
            .and_then(|value| value.checked_add(65536))
            .ok_or(AetherError::InvalidArgument)?
    }
    .max(1);

    let mut output = vec![0_u8; capacity];
    let mut written = 0_usize;
    let mut status = unsafe {
        ffi::aether_compress(
            input.as_ptr(),
            input.len(),
            &native,
            output.as_mut_ptr(),
            output.len(),
            &mut written,
        )
    };
    if status == ffi::AETHER_ERR_BUFFER_TOO_SMALL {
        output = vec![0_u8; written];
        status = unsafe {
            ffi::aether_compress(
                input.as_ptr(),
                input.len(),
                &native,
                output.as_mut_ptr(),
                output.len(),
                &mut written,
            )
        };
    }
    if status != ffi::AETHER_OK {
        return Err(status_error(status, written));
    }
    output.truncate(written);
    Ok(output)
}

pub fn decompress(input: &[u8], expected_len: usize) -> Result<Vec<f32>, AetherError> {
    if input.is_empty() {
        return Err(AetherError::InvalidArgument);
    }
    let mut output = vec![0.0_f32; expected_len.max(1)];
    let mut written = 0_usize;
    let status = unsafe {
        ffi::aether_decompress(
            input.as_ptr(),
            input.len(),
            output.as_mut_ptr(),
            expected_len,
            &mut written,
        )
    };
    if status != ffi::AETHER_OK {
        return Err(status_error(status, written));
    }
    if written != expected_len {
        return Err(AetherError::ExpectedSampleCount {
            expected: expected_len,
            actual: written,
        });
    }
    output.truncate(written);
    Ok(output)
}

pub fn decompress_slice(input: &[u8], start: usize, count: usize) -> Result<Vec<f32>, AetherError> {
    if input.is_empty() {
        return Err(AetherError::InvalidArgument);
    }
    let mut output = vec![0.0_f32; count.max(1)];
    let mut written = 0_usize;
    let status = unsafe {
        ffi::aether_decompress_slice(
            input.as_ptr(),
            input.len(),
            start,
            count,
            output.as_mut_ptr(),
            count,
            &mut written,
        )
    };
    if status != ffi::AETHER_OK {
        return Err(status_error(status, written));
    }
    output.truncate(written);
    Ok(output)
}

pub fn abi_version() -> u32 {
    unsafe { ffi::aether_c_abi_version() }
}

pub fn version() -> &'static str {
    unsafe { CStr::from_ptr(ffi::aether_version_string()) }
        .to_str()
        .expect("AetherStream version is valid UTF-8")
}
