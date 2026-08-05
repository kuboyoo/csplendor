"""Framework-neutral errors raised by API application services."""


class ApplicationError(RuntimeError):
    status_code = 500

    def __init__(self, detail: str):
        super().__init__(detail)
        self.detail = detail


class ResourceNotFound(ApplicationError):
    status_code = 404


class InvalidRequest(ApplicationError):
    status_code = 400


class PayloadTooLarge(ApplicationError):
    status_code = 413


class OptionalIntegrationUnavailable(ApplicationError):
    status_code = 503
