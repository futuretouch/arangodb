@startDocuBlock get_api_foxx_configuration

@RESTHEADER{GET /_api/foxx/configuration, Get the configuration options, getFoxxConfiguration}

@RESTDESCRIPTION
Fetches the current configuration for the service at the given mount path.

Returns an object mapping the configuration option names to their definitions
including a human-friendly `title` and the `current` value (if any).

@RESTQUERYPARAMETERS

@RESTQUERYPARAM{mount,string,required}
Mount path of the installed service.

@RESTRETURNCODES

@RESTRETURNCODE{200}
Returned if the request was successful.

@endDocuBlock
